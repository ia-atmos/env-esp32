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
//                  (a repeat repeats the last frame, as on the board, and
//                  with no frame before it counts as invalid)
//      <ms> espnow <mac> <data>                    a message arrives
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
// A send of ESP-NOW succeeds at once; the preferences live in memory.
//
// The validation follows the ESP32 (the original): pins 0-39, of which
// 34-39 are input only, 8 PWM channels, resolutions of 1-20 bits.

#define _DEFAULT_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

static bool queue_peek (queue_t* q, int64_t* t) {
    if (q->n == 0) {
        return false;
    }
    *t = q->evts[q->head].time_us;
    return true;
}

static bool peek_gpio (int64_t* t) { return queue_peek(&Q_GPIO, t); }
static bool peek_ir   (int64_t* t) { return queue_peek(&Q_IR,   t); }

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
    drv_source_add(DRV_SRC_GPIO, peek_gpio);
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

typedef enum { P_FADE, P_PIN, P_IR, P_ESPNOW, P_QUIT } pending_kind_t;

typedef struct {
    int64_t at;
    long seq;           // keeps the order of what happens at the same time
    pending_kind_t kind;
    int a, b;           // fade: channel; pin: pin, level
    drv_evt_t ir;
    drv_espnow_evt_t now;
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
static drv_evt_t IR_LAST;
static bool IR_HAVE_LAST = false;

#define ESPNOW_QUEUE_LEN 16
static bool ESPNOW_ON = false;
static drv_espnow_evt_t Q_NOW[ESPNOW_QUEUE_LEN];
static int Q_NOW_HEAD = 0, Q_NOW_N = 0;

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
            // as the board (drv/esp32/ir.c): a repeat code repeats the
            // last frame of the same protocol, and with none it is invalid
            if (IR_ON) {
                IR_STATS.captured++;
                if (p.ir.u.ir.repeat) {
                    if (!IR_HAVE_LAST ||
                        strcmp(IR_LAST.u.ir.protocol, p.ir.u.ir.protocol)) {
                        IR_STATS.invalid++;
                        break;
                    }
                    p.ir = IR_LAST;
                    p.ir.u.ir.repeat = true;
                } else {
                    IR_LAST = p.ir;
                    IR_HAVE_LAST = true;
                }
                p.ir.time_us = NOW;
                if (queue_put(&Q_IR, p.ir)) {
                    IR_STATS.decoded++;
                } else {
                    IR_STATS.dropped++;
                }
            }
            break;
        case P_ESPNOW:
            if (ESPNOW_ON && Q_NOW_N<ESPNOW_QUEUE_LEN) {
                p.now.time_us = NOW;
                Q_NOW[(Q_NOW_HEAD + Q_NOW_N++) % ESPNOW_QUEUE_LEN] = p.now;
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
        } else if (!strcmp(what, "espnow")) {
            int len = 0;
            p.kind = P_ESPNOW;
            p.now.kind = DRV_ESPNOW_RECV;
            p.now.ok = true;
            if (sscanf(rest, "%17s %n", p.now.mac, &len) < 1) {
                script_fail(path, n, line);
            }
            const char* data = rest + len;
            p.now.len = (int) strlen(data);
            if (p.now.len > DRV_ESPNOW_MAX_DATA) {
                script_fail(path, n, line);
            }
            memcpy(p.now.data, data, p.now.len);
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
    if (Q_GPIO.n>0 || Q_IR.n>0 || Q_NOW_N>0) {
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
    pins_init();
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
    drv_source_add(DRV_SRC_IR, peek_ir);
    IR_HAVE_LAST = false;
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
// ESP-NOW
///////////////////////////////////////////////////////////////////////////////

#define HOST_MAC "24:0a:c4:00:00:01"

static int CHANNEL = 1;
static char PEERS[20][18];
static int NPEERS = 0;

static bool valid_mac (const char* mac) {
    unsigned v[6];
    char end;
    return sscanf(mac, "%x:%x:%x:%x:%x:%x%c",
                  &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&end) == 6;
}

static bool peek_espnow (int64_t* t) {
    if (Q_NOW_N == 0) {
        return false;
    }
    *t = Q_NOW[Q_NOW_HEAD].time_us;
    return true;
}

const char* drv_espnow_init (void) {
    if (!ESPNOW_ON) {
        drv_source_add(DRV_SRC_ESPNOW, peek_espnow);
        trace("espnow.init");
        ESPNOW_ON = true;
        strcpy(PEERS[NPEERS++], "ff:ff:ff:ff:ff:ff");
    }
    return NULL;
}

const char* drv_espnow_add_peer (const char* mac) {
    if (!ESPNOW_ON) {
        return drv_errf("espnow.add_peer : esp-now is not initialized");
    }
    if (!valid_mac(mac)) {
        return drv_errf("espnow.add_peer : invalid MAC address format '%s'", mac);
    }
    for (int i=0; i<NPEERS; i++) {
        if (!strcasecmp(PEERS[i], mac)) {
            return NULL;
        }
    }
    if (NPEERS == 20) {
        return drv_errf("espnow.add_peer : failed to add esp-now peer: ESP_ERR_ESPNOW_FULL");
    }
    snprintf(PEERS[NPEERS++], 18, "%s", mac);
    trace("espnow.add_peer %s", mac);
    return NULL;
}

const char* drv_espnow_send (const char* mac, const uint8_t* data, int len) {
    if (!ESPNOW_ON) {
        return drv_errf("espnow.send : esp-now is not initialized");
    }
    if (!valid_mac(mac)) {
        return drv_errf("espnow.send : invalid MAC address format '%s'", mac);
    }
    if (len > DRV_ESPNOW_MAX_DATA) {
        return drv_errf("espnow.send : data length %d exceeds max ESP-NOW "
                        "payload of %d bytes", len, DRV_ESPNOW_MAX_DATA);
    }
    bool peer = false;
    for (int i=0; i<NPEERS; i++) {
        peer = peer || !strcasecmp(PEERS[i], mac);
    }
    if (!peer) {
        return drv_errf("espnow.send : esp_now_send failed: ESP_ERR_ESPNOW_NOT_FOUND");
    }
    trace("espnow.send %s %.*s", mac, len, (const char*)data);
    pending_t p = { .at=NOW, .kind=P_ESPNOW };
    p.now.kind = DRV_ESPNOW_SEND;
    p.now.ok   = true;
    snprintf(p.now.mac, sizeof(p.now.mac), "%s", mac);
    pending_add(p);
    return NULL;
}

const char* drv_espnow_get_mac (char mac[18]) {
    strcpy(mac, HOST_MAC);
    return NULL;
}

const char* drv_espnow_get_channel (int* channel) {
    *channel = CHANNEL;
    return NULL;
}

const char* drv_espnow_set_channel (int channel) {
    if (channel<1 || channel>14) {
        return drv_errf("espnow.set_channel : invalid Wi-Fi channel (1-14)");
    }
    CHANNEL = channel;
    trace("espnow.set_channel %d", channel);
    return NULL;
}

bool drv_espnow_next (drv_espnow_evt_t* evt) {
    if (Q_NOW_N == 0) {
        return false;
    }
    *evt = Q_NOW[Q_NOW_HEAD];
    Q_NOW_HEAD = (Q_NOW_HEAD + 1) % ESPNOW_QUEUE_LEN;
    Q_NOW_N--;
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// PREFERENCES: in memory
///////////////////////////////////////////////////////////////////////////////

typedef enum { NV_INT, NV_BOOL, NV_STR } nv_type_t;

typedef struct {
    bool used;
    char ns[16];
    char key[16];
    nv_type_t type;
    int32_t i;
    char* s;
} nv_t;

#define NV_MAX 64
static nv_t NV[NV_MAX];
static char NS[16] = "";        // the open namespace, or ""
static bool NS_RO = false;

static const char* nv_check (const char* fn, const char* key, bool write) {
    if (NS[0] == '\0') {
        return drv_errf("%s : no namespace is open (call preferences.open first)", fn);
    }
    size_t n = strlen(key);
    if (n<1 || n>15) {
        return drv_errf("%s : key length must be between 1 and 15 characters", fn);
    }
    if (write && NS_RO) {
        return drv_errf("%s : failed to write '%s': ESP_ERR_NVS_READ_ONLY", fn, key);
    }
    return NULL;
}

static nv_t* nv_find (const char* key) {
    for (int i=0; i<NV_MAX; i++) {
        if (NV[i].used && !strcmp(NV[i].ns,NS) && !strcmp(NV[i].key,key)) {
            return &NV[i];
        }
    }
    return NULL;
}

static nv_t* nv_new (const char* fn, const char* key, nv_type_t type, const char** err) {
    nv_t* v = nv_find(key);
    if (v == NULL) {
        for (int i=0; i<NV_MAX && v==NULL; i++) {
            if (!NV[i].used) {
                v = &NV[i];
            }
        }
        if (v == NULL) {
            *err = drv_errf("%s : failed to write '%s': ESP_ERR_NVS_NOT_ENOUGH_SPACE", fn, key);
            return NULL;
        }
        v->used = true;
        strcpy(v->ns, NS);
        strcpy(v->key, key);
    }
    free(v->s);
    v->s = NULL;
    v->type = type;
    return v;
}

const char* drv_prefs_open (const char* ns, bool readonly, const char* part) {
    size_t n = strlen(ns);
    if (n<1 || n>15) {
        return drv_errf("preferences.open : namespace length must be between 1 and 15 characters");
    }
    strcpy(NS, ns);
    NS_RO = readonly;
    trace("preferences.open %s%s%s%s", ns, readonly ? " readonly" : "",
          part ? " " : "", part ? part : "");
    return NULL;
}

const char* drv_prefs_close (void) {
    if (NS[0] != '\0') {
        trace("preferences.close %s", NS);
    }
    NS[0] = '\0';
    return NULL;
}

const char* drv_prefs_commit (void) {
    const char* e = (NS[0]=='\0') ? nv_check("preferences.commit", "", false) : NULL;
    return e;
}

const char* drv_prefs_put_int (const char* key, int32_t i, bool commit) {
    (void) commit;
    const char* e = nv_check("preferences.put_int", key, true);
    nv_t* v = (e == NULL) ? nv_new("preferences.put_int", key, NV_INT, &e) : NULL;
    if (v != NULL) {
        v->i = i;
        trace("preferences.put_int %s %d", key, (int)i);
    }
    return e;
}

const char* drv_prefs_get_int (const char* key, bool* found, int32_t* i) {
    const char* e = nv_check("preferences.get_int", key, false);
    nv_t* v = (e == NULL) ? nv_find(key) : NULL;
    *found = (v!=NULL && v->type==NV_INT);
    if (*found) {
        *i = v->i;
    } else if (v != NULL) {
        e = drv_errf("preferences.get_int : failed to read int '%s': "
                     "ESP_ERR_NVS_TYPE_MISMATCH", key);
    }
    return e;
}

const char* drv_prefs_put_bool (const char* key, bool b, bool commit) {
    (void) commit;
    const char* e = nv_check("preferences.put_bool", key, true);
    nv_t* v = (e == NULL) ? nv_new("preferences.put_bool", key, NV_BOOL, &e) : NULL;
    if (v != NULL) {
        v->i = b;
        trace("preferences.put_bool %s %s", key, b ? "true" : "false");
    }
    return e;
}

const char* drv_prefs_get_bool (const char* key, bool* found, bool* b) {
    const char* e = nv_check("preferences.get_bool", key, false);
    nv_t* v = (e == NULL) ? nv_find(key) : NULL;
    *found = (v!=NULL && v->type==NV_BOOL);
    if (*found) {
        *b = v->i;
    } else if (v != NULL) {
        e = drv_errf("preferences.get_bool : failed to read bool '%s': "
                     "ESP_ERR_NVS_TYPE_MISMATCH", key);
    }
    return e;
}

const char* drv_prefs_put_str (const char* key, const char* s, bool commit) {
    (void) commit;
    const char* e = nv_check("preferences.put_string", key, true);
    nv_t* v = (e == NULL) ? nv_new("preferences.put_string", key, NV_STR, &e) : NULL;
    if (v != NULL) {
        v->s = strdup(s);
        trace("preferences.put_string %s %s", key, s);
    }
    return e;
}

const char* drv_prefs_get_str (const char* key, bool* found, const char** s) {
    const char* e = nv_check("preferences.get_string", key, false);
    nv_t* v = (e == NULL) ? nv_find(key) : NULL;
    *found = (v!=NULL && v->type==NV_STR);
    if (*found) {
        *s = v->s;
    } else if (v != NULL) {
        e = drv_errf("preferences.get_string : failed to read string '%s': "
                     "ESP_ERR_NVS_TYPE_MISMATCH", key);
    }
    return e;
}

const char* drv_prefs_is_key (const char* key, bool* yes) {
    const char* e = nv_check("preferences.is_key", key, false);
    *yes = (e == NULL) && (nv_find(key) != NULL);
    return e;
}

const char* drv_prefs_remove (const char* key, bool commit) {
    (void) commit;
    const char* e = nv_check("preferences.remove", key, true);
    nv_t* v = (e == NULL) ? nv_find(key) : NULL;
    if (v != NULL) {
        free(v->s);
        memset(v, 0, sizeof(*v));
        trace("preferences.remove %s", key);
    }
    return e;
}

const char* drv_prefs_clear (bool commit) {
    (void) commit;
    const char* e = nv_check("preferences.clear", "x", true);
    if (e == NULL) {
        for (int i=0; i<NV_MAX; i++) {
            if (NV[i].used && !strcmp(NV[i].ns, NS)) {
                free(NV[i].s);
                memset(&NV[i], 0, sizeof(NV[i]));
            }
        }
        trace("preferences.clear %s", NS);
    }
    return e;
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
