// The drivers simulated on the host, for tests with no board.
//
// The clock is virtual: a wait jumps to the next thing that can happen
// (a timer of the program, the end of a fade, a line of the script),
// so a test runs at once and gives the same output every time. With
// ESP32_HOST_REALTIME set, a wait also sleeps for real.
//
// Input comes from the script in ESP32_HOST_SCRIPT, one line each:
//
//      <ms> pin <pin> <level>                      a pin goes to a level
//      <ms> ir <protocol> <address> <command> [repeat]
//      <ms> quit                                   nothing more happens
//      # a comment
//
// <ms> is the time since boot. After the last line, or after `quit`,
// only the timers of the program are left: when none is left either,
// `drv_clock_wait` gives -1 and the program ends.
//
// With ESP32_HOST_TRACE set, each action of the program on a driver is
// printed as `[drv <ms>] ...`.
//
// The validation follows the ESP32 (the original): pins 0-39, of which
// 34-39 are input only, 8 PWM channels, resolutions of 1-20 bits.

#define _POSIX_C_SOURCE 199309L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../drv.h"

#define PIN_COUNT     40
#define PWM_CHANNELS  8
#define QUEUE_LEN     32
#define PENDING_MAX   256

///////////////////////////////////////////////////////////////////////////////
// TRACE
///////////////////////////////////////////////////////////////////////////////

static int64_t NOW = 0;     // virtual microseconds since boot
static int TRACE = -1;

static void trace (const char* fmt, ...) {
    if (TRACE < 0) {
        TRACE = (getenv("ESP32_HOST_TRACE") != NULL);
    }
    if (!TRACE) {
        return;
    }
    printf("[drv %lldms] ", (long long)(NOW / 1000));
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

///////////////////////////////////////////////////////////////////////////////
// QUEUES OF EVENTS
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    drv_evt_t evts[QUEUE_LEN];
    int head;
    int n;
} queue_t;

static queue_t Q_GPIO;
static queue_t Q_IR;

static bool queue_put (queue_t* q, drv_evt_t evt) {
    if (q->n == QUEUE_LEN) {
        return false;
    }
    q->evts[(q->head + q->n) % QUEUE_LEN] = evt;
    q->n++;
    return true;
}

static bool queue_get (queue_t* q, drv_evt_t* evt) {
    if (q->n == 0) {
        return false;
    }
    *evt = q->evts[q->head];
    q->head = (q->head + 1) % QUEUE_LEN;
    q->n--;
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// STATE
///////////////////////////////////////////////////////////////////////////////

typedef enum { PIN_OFF, PIN_IN, PIN_OUT } pin_mode_t;

static struct {
    pin_mode_t mode;
    int level;
    int watch;      // 0: no; 1: rising; 2: falling; 3: both
    int pwm;        // channel, or -1
} PINS[PIN_COUNT];

static struct {
    bool configured;
    int resolution;
    int duty;
} PWM[PWM_CHANNELS];

static bool PINS_INIT = false;

static void pins_init (void) {
    if (PINS_INIT) {
        return;
    }
    for (int i=0; i<PIN_COUNT; i++) {
        PINS[i].pwm = -1;
    }
    PINS_INIT = true;
}

static bool valid_pin (int pin) {
    return pin>=0 && pin<PIN_COUNT;
}

static bool valid_output_pin (int pin) {
    return pin>=0 && pin<34;
}

static const char* bad_pin (const char* fn, int pin) {
    return drv_errf("%s : invalid GPIO pin %d", fn, pin);
}

static const char* bad_output (const char* fn, int pin) {
    return drv_errf("%s : GPIO pin %d cannot be used as output", fn, pin);
}

static const char* bad_channel (const char* fn, int ch) {
    if (ch<0 || ch>=PWM_CHANNELS) {
        return drv_errf("%s : invalid LEDC channel %d", fn, ch);
    }
    if (!PWM[ch].configured) {
        return drv_errf("%s : PWM channel %d has not been configured "
                        "(call setup_pwm first)", fn, ch);
    }
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// PENDING: what happens at a future time
///////////////////////////////////////////////////////////////////////////////

typedef enum { P_FADE, P_PIN, P_IR, P_QUIT } pending_kind_t;

typedef struct {
    int64_t at;
    long seq;           // keeps the order of what happens at the same time
    pending_kind_t kind;
    int a, b;           // fade: channel; pin: pin, level
    drv_evt_t ir;
} pending_t;

static pending_t PEND[PENDING_MAX];
static int  NPEND = 0;
static long SEQ = 0;
static bool QUIT = false;

static void pending_add (pending_t p) {
    if (NPEND == PENDING_MAX) {
        fprintf(stderr, "drv/host : too many pending events\n");
        exit(1);
    }
    p.seq = SEQ++;
    PEND[NPEND++] = p;
}

static int pending_next (void) {
    int k = -1;
    for (int i=0; i<NPEND; i++) {
        if (k==-1 || PEND[i].at<PEND[k].at ||
            (PEND[i].at==PEND[k].at && PEND[i].seq<PEND[k].seq)) {
            k = i;
        }
    }
    return k;
}

static void pin_change (int pin, int level) {
    int old = PINS[pin].level;
    PINS[pin].level = level;
    int w = PINS[pin].watch;
    if (old == level || w == 0) {
        return;
    }
    if ((level && (w & 1)) || (!level && (w & 2))) {
        drv_evt_t e = { .kind=DRV_EVT_GPIO, .time_us=NOW };
        e.u.gpio.pin   = pin;
        e.u.gpio.value = level;
        queue_put(&Q_GPIO, e);
    }
}

static bool IR_ON = false;
static drv_ir_stats_t IR_STATS;

static void pending_fire (pending_t p) {
    switch (p.kind) {
        case P_FADE: {
            drv_evt_t e = { .kind=DRV_EVT_PWM_FADE, .time_us=NOW };
            e.u.fade.channel = p.a;
            queue_put(&Q_GPIO, e);
            break;
        }
        case P_PIN:
            pins_init();
            pin_change(p.a, p.b);
            break;
        case P_IR:
            if (IR_ON) {
                p.ir.time_us = NOW;
                IR_STATS.captured++;
                if (queue_put(&Q_IR, p.ir)) {
                    IR_STATS.decoded++;
                } else {
                    IR_STATS.dropped++;
                }
            }
            break;
        case P_QUIT:
            QUIT = true;
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////
// SCRIPT
///////////////////////////////////////////////////////////////////////////////

static bool SCRIPT_READ = false;

static void script_fail (const char* path, int n, const char* line) {
    fprintf(stderr, "drv/host : %s:%d : invalid line : %s\n", path, n, line);
    exit(1);
}

static void script_read (void) {
    if (SCRIPT_READ) {
        return;
    }
    SCRIPT_READ = true;
    const char* path = getenv("ESP32_HOST_SCRIPT");
    if (path == NULL) {
        return;
    }
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "drv/host : cannot open %s\n", path);
        exit(1);
    }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        n++;
        line[strcspn(line, "\n")] = '\0';
        char* s = line + strspn(line, " \t");
        if (*s=='\0' || *s=='#') {
            continue;
        }
        long ms;
        char what[16];
        int off;
        if (sscanf(s, "%ld %15s %n", &ms, what, &off) < 2) {
            script_fail(path, n, line);
        }
        pending_t p = { .at = (int64_t)ms * 1000 };
        const char* rest = s + off;
        if (!strcmp(what, "pin")) {
            p.kind = P_PIN;
            if (sscanf(rest, "%d %d", &p.a, &p.b)!=2 || !valid_pin(p.a)) {
                script_fail(path, n, line);
            }
            p.b = !!p.b;
        } else if (!strcmp(what, "ir")) {
            static const char* protos[] = { "nec", "samsung", "sony", "lg" };
            char proto[16], rep[16] = "";
            long addr, cmd;
            if (sscanf(rest, "%15s %li %li %15s", proto, &addr, &cmd, rep) < 3
                || addr < 0 || cmd < 0) {
                script_fail(path, n, line);
            }
            p.kind = P_IR;
            p.ir.kind = DRV_EVT_IR;
            p.ir.u.ir.protocol = NULL;
            for (int i=0; i<4; i++) {
                if (!strcmp(proto, protos[i])) {
                    p.ir.u.ir.protocol = protos[i];
                }
            }
            if (p.ir.u.ir.protocol == NULL) {
                script_fail(path, n, line);
            }
            p.ir.u.ir.address = (uint32_t)addr;
            p.ir.u.ir.command = (uint32_t)cmd;
            p.ir.u.ir.bits    = 32;
            p.ir.u.ir.raw     = ((uint64_t)addr << 16) | (cmd & 0xFFFF);
            p.ir.u.ir.repeat  = !strcmp(rep, "repeat");
        } else if (!strcmp(what, "quit")) {
            p.kind = P_QUIT;
        } else {
            script_fail(path, n, line);
        }
        pending_add(p);
    }
    fclose(f);
}

///////////////////////////////////////////////////////////////////////////////
// CLOCK
///////////////////////////////////////////////////////////////////////////////

int64_t drv_clock_now (void) {
    return NOW;
}

void drv_clock_wake (void) {
}

static void sleep_us (int64_t us) {
    if (us <= 0 || getenv("ESP32_HOST_REALTIME") == NULL) {
        return;
    }
    struct timespec ts;
    ts.tv_sec  = (time_t) (us / 1000000);
    ts.tv_nsec = (long) ((us % 1000000) * 1000);
    nanosleep(&ts, NULL);
}

int64_t drv_clock_wait (int64_t max_us) {
    script_read();
    if (QUIT) {
        return -1;
    }
    if (Q_GPIO.n>0 || Q_IR.n>0) {
        return 0;           // the secondaries have events to emit
    }
    int k = pending_next();
    int64_t to;
    if (k == -1) {
        if (max_us < 0) {
            return -1;      // nothing can happen anymore
        }
        to = NOW + max_us;
    } else {
        to = PEND[k].at;
        if (max_us>=0 && NOW+max_us<to) {
            to = NOW + max_us;
        }
    }
    if (to < NOW) {
        to = NOW;
    }
    int64_t dt = to - NOW;
    sleep_us(dt);
    NOW = to;

    // what happens until now, in order
    while (1) {
        k = pending_next();
        if (k==-1 || PEND[k].at>NOW) {
            break;
        }
        pending_t p = PEND[k];
        PEND[k] = PEND[--NPEND];
        pending_fire(p);
    }
    return dt;
}

///////////////////////////////////////////////////////////////////////////////
// GPIO
///////////////////////////////////////////////////////////////////////////////

const char* drv_gpio_setup (int pin, const char* mode) {
    pins_init();
    if (!valid_pin(pin)) {
        return bad_pin("gpio.setup", pin);
    }
    pin_mode_t m;
    if (!strcmp(mode,"input") || !strcmp(mode,"input_pullup") ||
        !strcmp(mode,"input_pulldown")) {
        m = PIN_IN;
    } else if (!strcmp(mode,"output") || !strcmp(mode,"open_drain")) {
        if (!valid_output_pin(pin)) {
            return bad_output("gpio.setup", pin);
        }
        m = PIN_OUT;
    } else if (!strcmp(mode, "pwm")) {
        return drv_errf("gpio.setup : invalid gpio mode 'pwm'; "
                        "use gpio.setup_pwm and gpio.attach_pwm instead");
    } else {
        return drv_errf("gpio.setup : invalid gpio mode '%s'", mode);
    }
    PINS[pin].mode = m;
    PINS[pin].pwm  = -1;
    if (!strcmp(mode, "input_pullup")) {
        PINS[pin].level = 1;
    } else if (!strcmp(mode, "input_pulldown")) {
        PINS[pin].level = 0;
    }
    trace("gpio.setup %d %s", pin, mode);
    return NULL;
}

const char* drv_gpio_read (int pin, int* level) {
    pins_init();
    if (!valid_pin(pin)) {
        return bad_pin("gpio.read", pin);
    }
    *level = PINS[pin].level;
    return NULL;
}

const char* drv_gpio_write (int pin, int level) {
    pins_init();
    if (!valid_output_pin(pin)) {
        return bad_output("gpio.write", pin);
    }
    PINS[pin].level = !!level;
    trace("gpio.write %d %d", pin, !!level);
    return NULL;
}

const char* drv_gpio_watch (int pin, const char* edge) {
    pins_init();
    if (!valid_pin(pin)) {
        return bad_pin("gpio.watch", pin);
    }
    int w;
    if (!strcmp(edge, "rising")) {
        w = 1;
    } else if (!strcmp(edge, "falling")) {
        w = 2;
    } else if (!strcmp(edge, "both") || !strcmp(edge, "change")) {
        w = 3;
    } else {
        return drv_errf("gpio.watch : invalid edge '%s'", edge);
    }
    PINS[pin].watch = w;
    trace("gpio.watch %d %s", pin, edge);
    return NULL;
}

const char* drv_gpio_unwatch (int pin) {
    pins_init();
    if (!valid_pin(pin)) {
        return bad_pin("gpio.unwatch", pin);
    }
    PINS[pin].watch = 0;
    trace("gpio.unwatch %d", pin);
    return NULL;
}

const char* drv_gpio_read_adc (int pin, const char* atten, int* raw) {
    // the pins of ADC1 and ADC2 on the ESP32
    static const int adc[] = { 0,2,4,12,13,14,15,25,26,27,32,33,34,35,36,39 };
    bool ok = false;
    for (size_t i=0; i<sizeof(adc)/sizeof(adc[0]); i++) {
        ok = ok || (adc[i] == pin);
    }
    if (!ok) {
        return drv_errf("gpio.read_adc : GPIO pin %d is not supported by ADC", pin);
    }
    if (atten!=NULL && strcmp(atten,"0db") && strcmp(atten,"2.5db") &&
        strcmp(atten,"6db") && strcmp(atten,"11db") && strcmp(atten,"12db")) {
        return drv_errf("gpio.read_adc : invalid attenuation "
                        "(expected 0db, 2.5db, 6db, 11db, 12db)");
    }
    pins_init();
    *raw = PINS[pin].level ? 4095 : 0;
    return NULL;
}

const char* drv_pwm_setup (int ch, int freq, int res) {
    if (ch<0 || ch>=PWM_CHANNELS) {
        return drv_errf("gpio.setup_pwm : invalid LEDC channel %d", ch);
    }
    if (freq <= 0) {
        return drv_errf("gpio.setup_pwm : frequency must be positive");
    }
    if (res<1 || res>20) {
        return drv_errf("gpio.setup_pwm : resolution must be between 1 and 20");
    }
    PWM[ch].configured = true;
    PWM[ch].resolution = res;
    trace("gpio.setup_pwm %d %d %d", ch, freq, res);
    return NULL;
}

const char* drv_pwm_attach (int pin, int ch) {
    pins_init();
    if (!valid_output_pin(pin)) {
        return bad_output("gpio.attach_pwm", pin);
    }
    const char* e = bad_channel("gpio.attach_pwm", ch);
    if (e != NULL) {
        return e;
    }
    PINS[pin].mode = PIN_OUT;
    PINS[pin].pwm  = ch;
    trace("gpio.attach_pwm %d %d", pin, ch);
    return NULL;
}

const char* drv_pwm_detach (int pin) {
    pins_init();
    if (!valid_pin(pin)) {
        return bad_pin("gpio.detach_pwm", pin);
    }
    PINS[pin].mode = PIN_OFF;
    PINS[pin].pwm  = -1;
    trace("gpio.detach_pwm %d", pin);
    return NULL;
}

const char* drv_pwm_stop (int ch) {
    if (ch<0 || ch>=PWM_CHANNELS) {
        return drv_errf("gpio.stop_pwm : invalid LEDC channel %d", ch);
    }
    PWM[ch].duty = 0;
    trace("gpio.stop_pwm %d", ch);
    return NULL;
}

const char* drv_pwm_write (int ch, int duty) {
    const char* e = bad_channel("gpio.write_pwm", ch);
    if (e != NULL) {
        return e;
    }
    if (duty<0 || duty>(1<<PWM[ch].resolution)-1) {
        return drv_errf("gpio.write_pwm : duty out of range for channel resolution");
    }
    PWM[ch].duty = duty;
    trace("gpio.write_pwm %d %d", ch, duty);
    return NULL;
}

const char* drv_pwm_fade (int ch, int duty, int ms) {
    const char* e = bad_channel("gpio.fade_pwm", ch);
    if (e != NULL) {
        return e;
    }
    if (duty<0 || duty>(1<<PWM[ch].resolution)-1) {
        return drv_errf("gpio.fade_pwm : target_duty out of range for channel resolution");
    }
    if (ms < 0) {
        return drv_errf("gpio.fade_pwm : duration must be non-negative");
    }
    PWM[ch].duty = duty;
    trace("gpio.fade_pwm %d %d %d", ch, duty, ms);
    pending_t p = { .at=NOW+(int64_t)ms*1000, .kind=P_FADE, .a=ch };
    pending_add(p);
    return NULL;
}

bool drv_gpio_next (drv_evt_t* evt) {
    return queue_get(&Q_GPIO, evt);
}

///////////////////////////////////////////////////////////////////////////////
// IR
///////////////////////////////////////////////////////////////////////////////

const char* drv_ir_start (int pin, int64_t glitch_ns, int64_t idle_ns) {
    if (!valid_pin(pin)) {
        return bad_pin("ir.start", pin);
    }
    if (IR_ON) {
        return drv_errf("ir.start : IR receiver is already running");
    }
    if (glitch_ns < 0) {
        glitch_ns = 1250;
    }
    if (idle_ns < 0) {
        idle_ns = 15000000;
    }
    if (glitch_ns > 3187) {
        return drv_errf("ir.start : glitch_ns must be <= 3187 ns "
                        "(ESP32 hardware filter limit)");
    }
    if (glitch_ns >= idle_ns) {
        return drv_errf("ir.start : expected glitch_ns < idle_ns");
    }
    memset(&IR_STATS, 0, sizeof(IR_STATS));
    IR_ON = true;
    trace("ir.start %d", pin);
    return NULL;
}

const char* drv_ir_stop (void) {
    if (IR_ON) {
        trace("ir.stop");
    }
    IR_ON = false;
    Q_IR.n = 0;
    return NULL;
}

void drv_ir_stats (drv_ir_stats_t* stats) {
    *stats = IR_STATS;
}

bool drv_ir_next (drv_evt_t* evt) {
    return queue_get(&Q_IR, evt);
}

///////////////////////////////////////////////////////////////////////////////
// MEMORY: fixed numbers, close to an ESP32 with nothing running
///////////////////////////////////////////////////////////////////////////////

void drv_meminfo (drv_meminfo_t* m) {
    m->heap_total            = 320 * 1024;
    m->heap_free             = 280 * 1024;
    m->heap_min_free         = 270 * 1024;
    m->heap_largest_free     = 110 * 1024;
    m->internal_total        = 320 * 1024;
    m->internal_free         = 280 * 1024;
    m->internal_min_free     = 270 * 1024;
    m->internal_largest_free = 110 * 1024;
    m->stack_free            = 4096;
}
