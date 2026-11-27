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
//      0 hold <pin> <level>        a pin is at a level from boot (as a
//                  button held at reset): a pull-up or pull-down of the
//                  program does not change it
//      <ms> ir <protocol> <address> <command> [repeat]
//                  (a repeat repeats the last frame, as on the board, and
//                  with no frame before it counts as invalid)
//      <ms> espnow <mac> <data>                    a message arrives
//      <ms> wifi ap_add <ssid> <bssid> <rssi> <ch> <auth>   a visible AP
//      <ms> wifi scan_fail                         the scan in progress fails
//      <ms> wifi connected <ssid> <bssid> <ch>     STA associated
//      <ms> wifi got_ip <ip> <mask> <gw> [changed] STA obtained IP
//      <ms> wifi disconnected <reason> [ssid] [bssid]
//      <ms> wifi lost_ip                           STA lost IP
//      <ms> wifi ap_connected <mac> <aid>          client joined SoftAP
//      <ms> wifi ap_sta_ip <mac> <ip>              SoftAP client got IP
//      <ms> wifi ap_disconnected <mac> <aid> <reason>
//      0 sleep cause <name>                        simulated boot wakeup cause
//      0 sleep reset <reason>                      simulated reset reason
//      0 sleep wakeup_pin <pin>                    simulated ext1 wakeup pin
//      0 sleep rtc_int <key> <val>                 pre-populated RTC int
//      0 sleep rtc_bool <key> <true|false>         pre-populated RTC bool
//      0 sleep rtc_str <key> <val>                 pre-populated RTC string
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
// Wi-Fi and ESP-NOW share the radio, as on the board: it turns off
// (traced as `radio off`) only when neither uses it.
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
    bool held;      // the level comes from the script: pulls do not change it
} PINS[PIN_COUNT];

static struct {
    bool configured;
    int resolution;
    int duty;
} PWM[PWM_CHANNELS];

static bool PINS_INIT = false;
static void script_read (void);

static void pins_init (void) {
    if (PINS_INIT) {
        return;
    }
    drv_source_add(DRV_SRC_GPIO, peek_gpio);
    for (int i=0; i<PIN_COUNT; i++) {
        PINS[i].pwm = -1;
    }
    PINS_INIT = true;
    script_read();      // for its `hold` lines
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

typedef enum { P_FADE, P_PIN, P_IR, P_ESPNOW, P_WIFI, P_WIFI_SCAN, P_WIFI_SCAN_FAIL,
               P_QUIT } pending_kind_t;

typedef struct {
    int64_t at;
    long seq;           // keeps the order of what happens at the same time
    pending_kind_t kind;
    int a, b;           // fade: channel; pin: pin, level
    drv_evt_t ir;
    drv_espnow_evt_t now;
    drv_wifi_evt_t wifi;
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

#define WIFI_QUEUE_LEN 16
typedef enum {
    WST_STOPPED = 0,
    WST_DISCONNECTED,
    WST_CONNECTING,
    WST_CONNECTED,
    WST_GOT_IP,
} host_wifi_st_t;

static bool WIFI_INITED = false;
static bool WIFI_STA_ON = false;
static bool WIFI_AP_ON  = false;
static bool WIFI_SCANNING = false;
static host_wifi_st_t WIFI_ST = WST_STOPPED;
static char WIFI_SSID[33];
static char WIFI_BSSID[18];
static char WIFI_IP[16];
static char WIFI_MASK[16];
static char WIFI_GW[16];
static int  WIFI_CH = 0;
static int  WIFI_RSSI = -55;
static char WIFI_AP_SSID[33];
static char WIFI_AP_IP[16];
static int  WIFI_AP_CH = 0;
static int  CHANNEL = 1;            // shared radio channel (Wi-Fi + ESP-NOW)
static drv_wifi_evt_t Q_WIFI[WIFI_QUEUE_LEN];
static int Q_WIFI_HEAD = 0, Q_WIFI_N = 0;

static bool RADIO_ON = false;      // the Wi-Fi radio, shared with ESP-NOW

// The SoftAP clients that the program saw connect, as on the board: a
// stop disconnects them (drv/esp32/wifi.c).
#define AP_CLIENTS_MAX 10
static struct { char mac[18]; int aid; } AP_CLIENTS[AP_CLIENTS_MAX];
static int AP_CLIENTS_N = 0;

static drv_wifi_ap_t HOST_APS[DRV_WIFI_SCAN_MAX];
static int           HOST_APS_N = 0;
static drv_wifi_ap_t SCAN_APS[DRV_WIFI_SCAN_MAX];
static int           SCAN_APS_N = 0;
static char          SCAN_FILTER_SSID[33];
static int           SCAN_FILTER_CH = 0;
static int           SCAN_MAX_APS = 20;

// Sleep & RTC retention memory host state (declared before script_read)
static drv_sleep_wake_t SLEEP_WAKE = { .cause = "none" };
static const char*      SLEEP_RESET = "poweron";

#define HOST_RTC_SLOTS 32
typedef enum { HRTC_NONE = 0, HRTC_INT, HRTC_BOOL, HRTC_STR } host_rtc_type_t;
typedef struct {
    bool used;
    host_rtc_type_t type;
    char key[16];
    int64_t i;
    char s[DRV_RTC_STR_MAX];
} host_rtc_slot_t;
static host_rtc_slot_t HOST_RTC[HOST_RTC_SLOTS];

static const char* host_rtc_put_raw (const char* key, host_rtc_type_t type,
                                     int64_t i, const char* s) {
    for (int k = 0; k < HOST_RTC_SLOTS; k++) {
        if (HOST_RTC[k].used && !strcmp(HOST_RTC[k].key, key)) {
            HOST_RTC[k].type = type;
            HOST_RTC[k].i = i;
            if (s != NULL) {
                snprintf(HOST_RTC[k].s, sizeof(HOST_RTC[k].s), "%s", s);
            } else {
                HOST_RTC[k].s[0] = '\0';
            }
            return NULL;
        }
    }
    for (int k = 0; k < HOST_RTC_SLOTS; k++) {
        if (!HOST_RTC[k].used) {
            HOST_RTC[k].used = true;
            HOST_RTC[k].type = type;
            snprintf(HOST_RTC[k].key, sizeof(HOST_RTC[k].key), "%s", key);
            HOST_RTC[k].i = i;
            if (s != NULL) {
                snprintf(HOST_RTC[k].s, sizeof(HOST_RTC[k].s), "%s", s);
            } else {
                HOST_RTC[k].s[0] = '\0';
            }
            return NULL;
        }
    }
    return "full";
}

static void wifi_q_put (drv_wifi_evt_t e) {
    if (Q_WIFI_N < WIFI_QUEUE_LEN) {
        Q_WIFI[(Q_WIFI_HEAD + Q_WIFI_N++) % WIFI_QUEUE_LEN] = e;
    }
}

static void ap_clients_remove (const char* mac) {
    for (int i = 0; i < AP_CLIENTS_N; i++) {
        if (!strcasecmp(AP_CLIENTS[i].mac, mac)) {
            AP_CLIENTS[i] = AP_CLIENTS[--AP_CLIENTS_N];
            return;
        }
    }
}

// A stop of the SoftAP: each client that the program saw disconnects.
static void ap_clients_gone (void) {
    for (int i = 0; i < AP_CLIENTS_N; i++) {
        drv_wifi_evt_t e = { .kind = DRV_WIFI_AP_DISCONNECTED, .time_us = NOW,
                             .aid = AP_CLIENTS[i].aid, .reason = 8 };
        strcpy(e.bssid, AP_CLIENTS[i].mac);
        wifi_q_put(e);
    }
    AP_CLIENTS_N = 0;
}

static void wifi_scan_finish (void) {
    if (!WIFI_SCANNING) {
        return;
    }
    WIFI_SCANNING = false;
    SCAN_APS_N = 0;
    for (int i = 0; i < HOST_APS_N; i++) {
        if (SCAN_FILTER_SSID[0] != '\0' && strcmp(HOST_APS[i].ssid, SCAN_FILTER_SSID)) {
            continue;
        }
        if (SCAN_FILTER_CH > 0 && HOST_APS[i].channel != SCAN_FILTER_CH) {
            continue;
        }
        SCAN_APS[SCAN_APS_N++] = HOST_APS[i];
    }
    // sort by RSSI descending (matches ESP-IDF)
    for (int i = 0; i < SCAN_APS_N; i++) {
        for (int j = i + 1; j < SCAN_APS_N; j++) {
            if (SCAN_APS[j].rssi > SCAN_APS[i].rssi) {
                drv_wifi_ap_t tmp = SCAN_APS[i];
                SCAN_APS[i] = SCAN_APS[j];
                SCAN_APS[j] = tmp;
            }
        }
    }
    if (SCAN_APS_N > SCAN_MAX_APS) {
        SCAN_APS_N = SCAN_MAX_APS;
    }
    drv_wifi_evt_t e = {
        .kind = DRV_WIFI_SCAN_DONE,
        .time_us = NOW,
        .scan_count = SCAN_APS_N,
        .scan_status = "success",
    };
    wifi_q_put(e);
}

// A scan that does not finish: "stopped" (wifi.stop) or "fail" (script).
static void wifi_scan_end (const char* status) {
    if (!WIFI_SCANNING) {
        return;
    }
    WIFI_SCANNING = false;
    SCAN_APS_N = 0;
    drv_wifi_evt_t e = { .kind = DRV_WIFI_SCAN_DONE, .time_us = NOW,
                         .scan_count = 0, .scan_status = status };
    wifi_q_put(e);
}

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
            PINS[p.a].held = true;
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
        case P_WIFI:
            if (!WIFI_INITED) {
                break;
            }
            p.wifi.time_us = NOW;
            switch (p.wifi.kind) {
                case DRV_WIFI_CONNECTED:
                    WIFI_ST = WST_CONNECTED;
                    strcpy(WIFI_SSID, p.wifi.ssid);
                    strcpy(WIFI_BSSID, p.wifi.bssid);
                    WIFI_CH = p.wifi.channel;
                    CHANNEL = p.wifi.channel;
                    break;
                case DRV_WIFI_DISCONNECTED:
                    if (p.wifi.ssid[0] == '\0') {
                        strcpy(p.wifi.ssid, WIFI_SSID);
                    }
                    if (p.wifi.bssid[0] == '\0') {
                        strcpy(p.wifi.bssid, WIFI_BSSID);
                    }
                    if (WIFI_ST != WST_STOPPED) {
                        WIFI_ST = WST_DISCONNECTED;
                    }
                    WIFI_SSID[0] = WIFI_BSSID[0] = '\0';
                    WIFI_IP[0] = WIFI_MASK[0] = WIFI_GW[0] = '\0';
                    WIFI_CH = 0;
                    break;
                case DRV_WIFI_GOT_IP:
                    WIFI_ST = WST_GOT_IP;
                    strcpy(WIFI_IP, p.wifi.ip);
                    strcpy(WIFI_MASK, p.wifi.netmask);
                    strcpy(WIFI_GW, p.wifi.gateway);
                    break;
                case DRV_WIFI_LOST_IP:
                    if (WIFI_ST == WST_GOT_IP) {
                        WIFI_ST = WST_CONNECTED;
                    }
                    WIFI_IP[0] = WIFI_MASK[0] = WIFI_GW[0] = '\0';
                    break;
                case DRV_WIFI_AP_CONNECTED:
                    if (!WIFI_AP_ON) {
                        return;         // no SoftAP: as the board, dropped
                    }
                    if (AP_CLIENTS_N < AP_CLIENTS_MAX) {
                        strcpy(AP_CLIENTS[AP_CLIENTS_N].mac, p.wifi.bssid);
                        AP_CLIENTS[AP_CLIENTS_N].aid = p.wifi.aid;
                        AP_CLIENTS_N++;
                    }
                    break;
                case DRV_WIFI_AP_DISCONNECTED:
                    if (!WIFI_AP_ON) {
                        return;         // the stop already disconnected it
                    }
                    ap_clients_remove(p.wifi.bssid);
                    break;
                case DRV_WIFI_AP_STA_IP:
                    if (!WIFI_AP_ON) {
                        return;
                    }
                    break;
                default:
                    break;
            }
            wifi_q_put(p.wifi);
            break;
        case P_WIFI_SCAN:
            wifi_scan_finish();
            break;
        case P_WIFI_SCAN_FAIL:
            wifi_scan_end("fail");
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
        if (!strcmp(what, "hold")) {
            int pin, level;
            if (ms!=0 || sscanf(rest, "%d %d", &pin, &level)!=2 || !valid_pin(pin)) {
                script_fail(path, n, line);
            }
            PINS[pin].level = !!level;
            PINS[pin].held  = true;
            continue;
        } else if (!strcmp(what, "pin")) {
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
        } else if (!strcmp(what, "wifi")) {
            char sub[24];
            int len = 0;
            if (sscanf(rest, "%23s %n", sub, &len) < 1) {
                script_fail(path, n, line);
            }
            const char* args = rest + len;
            if (!strcmp(sub, "ap_add")) {
                static const char* auths[] = {
                    "open", "wep", "wpa", "wpa2", "wpa_wpa2",
                    "enterprise", "wpa3", "wpa2_wpa3", "wapi", "owe", "unknown"
                };
                char ssid[33], bssid[18], auth[16];
                int rssi, ch;
                if (sscanf(args, "%32s %17s %d %d %15s", ssid, bssid, &rssi, &ch, auth) != 5 ||
                    HOST_APS_N >= DRV_WIFI_SCAN_MAX) {
                    script_fail(path, n, line);
                }
                const char* aname = NULL;
                for (size_t i = 0; i < sizeof(auths)/sizeof(auths[0]); i++) {
                    if (!strcmp(auth, auths[i])) {
                        aname = auths[i];
                    }
                }
                if (aname == NULL) {
                    script_fail(path, n, line);
                }
                drv_wifi_ap_t* a = &HOST_APS[HOST_APS_N++];
                strcpy(a->ssid, ssid);
                strcpy(a->bssid, bssid);
                a->rssi = rssi;
                a->channel = ch;
                a->auth = aname;
                continue;
            }
            if (!strcmp(sub, "scan_fail")) {
                p.kind = P_WIFI_SCAN_FAIL;
                pending_add(p);
                continue;
            }
            p.kind = P_WIFI;
            if (!strcmp(sub, "connected")) {
                p.wifi.kind = DRV_WIFI_CONNECTED;
                if (sscanf(args, "%32s %17s %d", p.wifi.ssid, p.wifi.bssid, &p.wifi.channel) != 3) {
                    script_fail(path, n, line);
                }
            } else if (!strcmp(sub, "got_ip")) {
                char chg[16] = "";
                p.wifi.kind = DRV_WIFI_GOT_IP;
                if (sscanf(args, "%15s %15s %15s %15s",
                           p.wifi.ip, p.wifi.netmask, p.wifi.gateway, chg) < 3) {
                    script_fail(path, n, line);
                }
                p.wifi.changed = !strcmp(chg, "changed");
            } else if (!strcmp(sub, "disconnected")) {
                p.wifi.kind = DRV_WIFI_DISCONNECTED;
                if (sscanf(args, "%d %32s %17s",
                           &p.wifi.reason, p.wifi.ssid, p.wifi.bssid) < 1) {
                    script_fail(path, n, line);
                }
            } else if (!strcmp(sub, "lost_ip")) {
                p.wifi.kind = DRV_WIFI_LOST_IP;
            } else if (!strcmp(sub, "ap_connected")) {
                p.wifi.kind = DRV_WIFI_AP_CONNECTED;
                if (sscanf(args, "%17s %d", p.wifi.bssid, &p.wifi.aid) != 2) {
                    script_fail(path, n, line);
                }
            } else if (!strcmp(sub, "ap_sta_ip")) {
                p.wifi.kind = DRV_WIFI_AP_STA_IP;
                if (sscanf(args, "%17s %15s", p.wifi.bssid, p.wifi.ip) != 2) {
                    script_fail(path, n, line);
                }
            } else if (!strcmp(sub, "ap_disconnected")) {
                p.wifi.kind = DRV_WIFI_AP_DISCONNECTED;
                if (sscanf(args, "%17s %d %d", p.wifi.bssid, &p.wifi.aid, &p.wifi.reason) != 3) {
                    script_fail(path, n, line);
                }
            } else {
                script_fail(path, n, line);
            }
        } else if (!strcmp(what, "sleep")) {
            char sub[24];
            int len = 0;
            if (ms != 0 || sscanf(rest, "%23s %n", sub, &len) < 1) {
                script_fail(path, n, line);
            }
            const char* args = rest + len;
            if (!strcmp(sub, "cause")) {
                static const char* causes[] = {
                    "none", "timer", "ext0", "ext1", "gpio",
                    "touch", "ulp", "uart", "wifi", "bt", "other"
                };
                char name[16];
                if (sscanf(args, "%15s", name) != 1) {
                    script_fail(path, n, line);
                }
                const char* found = NULL;
                for (size_t i = 0; i < sizeof(causes)/sizeof(causes[0]); i++) {
                    if (!strcmp(name, causes[i])) {
                        found = causes[i];
                    }
                }
                if (found == NULL) {
                    script_fail(path, n, line);
                }
                if (strcmp(found, "none") && SLEEP_WAKE.causes_n < DRV_SLEEP_MAX_CAUSES) {
                    SLEEP_WAKE.causes[SLEEP_WAKE.causes_n++] = found;
                    SLEEP_WAKE.cause = SLEEP_WAKE.causes[0];
                }
            } else if (!strcmp(sub, "reset")) {
                static const char* reasons[] = {
                    "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt",
                    "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag",
                    "efuse", "pwr_glitch", "cpu_lockup", "unknown"
                };
                char name[16];
                if (sscanf(args, "%15s", name) != 1) {
                    script_fail(path, n, line);
                }
                const char* found = NULL;
                for (size_t i = 0; i < sizeof(reasons)/sizeof(reasons[0]); i++) {
                    if (!strcmp(name, reasons[i])) {
                        found = reasons[i];
                    }
                }
                if (found == NULL) {
                    script_fail(path, n, line);
                }
                SLEEP_RESET = found;
            } else if (!strcmp(sub, "wakeup_pin")) {
                int p = -1;
                if (sscanf(args, "%d", &p) != 1 || !valid_pin(p) ||
                    SLEEP_WAKE.pins_n >= DRV_SLEEP_MAX_PINS) {
                    script_fail(path, n, line);
                }
                SLEEP_WAKE.pins[SLEEP_WAKE.pins_n++] = p;
            } else if (!strcmp(sub, "rtc_int")) {
                char key[16];
                long long v = 0;
                if (sscanf(args, "%15s %lld", key, &v) != 2 ||
                    host_rtc_put_raw(key, HRTC_INT, (int64_t)v, NULL) != NULL) {
                    script_fail(path, n, line);
                }
            } else if (!strcmp(sub, "rtc_bool")) {
                char key[16], b[16];
                if (sscanf(args, "%15s %15s", key, b) != 2 ||
                    (strcmp(b, "true") && strcmp(b, "false")) ||
                    host_rtc_put_raw(key, HRTC_BOOL, !strcmp(b, "true") ? 1 : 0, NULL) != NULL) {
                    script_fail(path, n, line);
                }
            } else if (!strcmp(sub, "rtc_str")) {
                char key[16];
                int koff = 0;
                if (sscanf(args, "%15s %n", key, &koff) < 1 ||
                    host_rtc_put_raw(key, HRTC_STR, 0, args + koff) != NULL) {
                    script_fail(path, n, line);
                }
            } else {
                script_fail(path, n, line);
            }
            continue;
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

void drv_clock_sync (void) {
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
    if (Q_GPIO.n>0 || Q_IR.n>0 || Q_NOW_N>0 || Q_WIFI_N>0) {
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
    if (PINS[pin].held) {
        // the level comes from the script
    } else if (!strcmp(mode, "input_pullup")) {
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

static void radio_on (void) {
    RADIO_ON = true;
}

// Turns the radio off if neither Wi-Fi nor ESP-NOW uses it anymore.
static void radio_check (void) {
    bool wifi = WIFI_STA_ON || WIFI_AP_ON || WIFI_SCANNING || WIFI_ST != WST_STOPPED;
    if (RADIO_ON && !ESPNOW_ON && !wifi) {
        RADIO_ON = false;
        trace("radio off");
    }
}

const char* drv_espnow_init (void) {
    if (!ESPNOW_ON) {
        drv_source_add(DRV_SRC_ESPNOW, peek_espnow);
        trace("espnow.init");
        ESPNOW_ON = true;
        radio_on();
        NPEERS = 0;
        strcpy(PEERS[NPEERS++], "ff:ff:ff:ff:ff:ff");
    }
    return NULL;
}

const char* drv_espnow_stop (const char* reason) {
    if (!ESPNOW_ON) {
        return NULL;
    }
    trace("espnow.stop %s", reason);
    ESPNOW_ON = false;
    NPEERS = 0;
    drv_espnow_evt_t e = { .kind=DRV_ESPNOW_STOP, .time_us=NOW, .reason=reason };
    if (Q_NOW_N == ESPNOW_QUEUE_LEN) {      // as the board: the oldest gives place
        Q_NOW_HEAD = (Q_NOW_HEAD + 1) % ESPNOW_QUEUE_LEN;
        Q_NOW_N--;
    }
    Q_NOW[(Q_NOW_HEAD + Q_NOW_N++) % ESPNOW_QUEUE_LEN] = e;
    radio_check();
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
// WI-FI
///////////////////////////////////////////////////////////////////////////////

#define HOST_AP_MAC "24:0a:c4:00:00:02"

static const char* host_wifi_state (void) {
    switch (WIFI_ST) {
        case WST_STOPPED:      return "stopped";
        case WST_DISCONNECTED: return "disconnected";
        case WST_CONNECTING:   return "connecting";
        case WST_CONNECTED:    return "connected";
        case WST_GOT_IP:       return "got_ip";
    }
    return "stopped";
}

static bool peek_wifi (int64_t* t) {
    if (Q_WIFI_N == 0) {
        return false;
    }
    *t = Q_WIFI[Q_WIFI_HEAD].time_us;
    return true;
}

static void host_wifi_init (void) {
    if (!WIFI_INITED) {
        drv_source_add(DRV_SRC_WIFI, peek_wifi);
        WIFI_INITED = true;
        script_read();
    }
}

const char* drv_wifi_connect (const char* ssid, const char* password,
                              const drv_wifi_connect_opts_t* opts) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return drv_errf("wifi.connect : ssid must be 1 to 32 characters");
    }
    if (password != NULL && strlen(password) > 64) {
        return drv_errf("wifi.connect : password must be at most 64 characters");
    }
    if (opts != NULL) {
        if (opts->bssid != NULL && !valid_mac(opts->bssid)) {
            return drv_errf("wifi.connect : invalid BSSID format '%s'", opts->bssid);
        }
        if (opts->channel < 0 || opts->channel > 14) {
            return drv_errf("wifi.connect : invalid Wi-Fi channel (1-14)");
        }
        if (opts->power_save != NULL &&
            strcmp(opts->power_save, "none") &&
            strcmp(opts->power_save, "min") &&
            strcmp(opts->power_save, "max")) {
            return drv_errf("wifi.connect : invalid power_save '%s' "
                            "(expected none, min, max)", opts->power_save);
        }
    }
    host_wifi_init();
    radio_on();
    WIFI_STA_ON = true;
    WIFI_ST = WST_CONNECTING;
    strcpy(WIFI_SSID, ssid);
    trace("wifi.connect %s", ssid);
    return NULL;
}

const char* drv_wifi_disconnect (void) {
    if (!WIFI_STA_ON || WIFI_ST == WST_STOPPED || WIFI_ST == WST_DISCONNECTED) {
        return NULL;
    }
    trace("wifi.disconnect");
    pending_t p = { .at = NOW, .kind = P_WIFI };
    p.wifi.kind = DRV_WIFI_DISCONNECTED;
    p.wifi.reason = 8;      // WIFI_REASON_ASSOC_LEAVE
    strcpy(p.wifi.ssid, WIFI_SSID);
    strcpy(p.wifi.bssid, WIFI_BSSID);
    pending_add(p);
    return NULL;
}

const char* drv_wifi_stop (void) {
    if (WIFI_ST != WST_STOPPED || WIFI_AP_ON) {
        trace("wifi.stop");
    }
    // as the board: a scan in progress ends with no AP, each SoftAP client
    // disconnects, and the STA that was connecting or connected disconnects
    wifi_scan_end("stopped");
    ap_clients_gone();
    if (WIFI_ST == WST_CONNECTING || WIFI_ST == WST_CONNECTED || WIFI_ST == WST_GOT_IP) {
        drv_wifi_evt_t e = { .kind = DRV_WIFI_DISCONNECTED, .time_us = NOW, .reason = 8 };
        strcpy(e.ssid, WIFI_SSID);
        strcpy(e.bssid, WIFI_BSSID);
        wifi_q_put(e);
    }
    WIFI_STA_ON = false;
    WIFI_AP_ON = false;
    WIFI_SCANNING = false;
    WIFI_ST = WST_STOPPED;
    WIFI_SSID[0] = WIFI_BSSID[0] = '\0';
    WIFI_IP[0] = WIFI_MASK[0] = WIFI_GW[0] = '\0';
    WIFI_AP_SSID[0] = WIFI_AP_IP[0] = '\0';
    WIFI_CH = 0;
    radio_check();
    return NULL;
}

const char* drv_wifi_scan (const drv_wifi_scan_opts_t* opts) {
    if (WIFI_SCANNING) {
        return drv_errf("wifi.scan : scan already in progress");
    }
    SCAN_FILTER_SSID[0] = '\0';
    SCAN_FILTER_CH = 0;
    SCAN_MAX_APS = 20;
    if (opts != NULL) {
        if (opts->ssid != NULL) {
            if (strlen(opts->ssid) > 32) {
                return drv_errf("wifi.scan : ssid must be at most 32 characters");
            }
            strcpy(SCAN_FILTER_SSID, opts->ssid);
        }
        if (opts->channel < 0 || opts->channel > 14) {
            return drv_errf("wifi.scan : invalid Wi-Fi channel (1-14)");
        }
        SCAN_FILTER_CH = opts->channel;
        if (opts->max_aps > 0) {
            SCAN_MAX_APS = (opts->max_aps > DRV_WIFI_SCAN_MAX) ? DRV_WIFI_SCAN_MAX : opts->max_aps;
        }
    }
    host_wifi_init();
    radio_on();
    if (WIFI_ST == WST_STOPPED) {
        WIFI_ST = WST_DISCONNECTED;
    }
    WIFI_SCANNING = true;
    trace("wifi.scan");
    pending_t p = { .at = NOW + 100000, .kind = P_WIFI_SCAN };
    pending_add(p);
    return NULL;
}

int drv_wifi_scan_result (int index, drv_wifi_ap_t* ap) {
    if (index < 0 || index >= SCAN_APS_N) {
        return 0;
    }
    *ap = SCAN_APS[index];
    return 1;
}

const char* drv_wifi_start_ap (const char* ssid, const char* password,
                               const drv_wifi_ap_opts_t* opts) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return drv_errf("wifi.start_ap : ssid must be 1 to 32 characters");
    }
    size_t plen = (password != NULL) ? strlen(password) : 0;
    if (plen > 0 && (plen < 8 || plen > 64)) {
        return drv_errf("wifi.start_ap : password must be 8 to 64 characters "
                        "(or empty for open AP)");
    }
    int ch = 1;
    if (opts != NULL) {
        if (opts->channel != 0) {
            if (opts->channel < 1 || opts->channel > 13) {
                return drv_errf("wifi.start_ap : invalid AP channel (1-13)");
            }
            ch = opts->channel;
        }
        if (opts->max_conn != 0 && (opts->max_conn < 1 || opts->max_conn > 10)) {
            return drv_errf("wifi.start_ap : max_conn must be between 1 and 10");
        }
    }
    host_wifi_init();
    radio_on();
    WIFI_AP_ON = true;
    strcpy(WIFI_AP_SSID, ssid);
    strcpy(WIFI_AP_IP, "192.168.4.1");
    WIFI_AP_CH = ch;
    if (WIFI_ST != WST_CONNECTED && WIFI_ST != WST_GOT_IP) {
        CHANNEL = ch;
    }
    if (WIFI_ST == WST_STOPPED) {
        WIFI_ST = WST_DISCONNECTED;
    }
    trace("wifi.start_ap %s %d", ssid, ch);
    return NULL;
}

const char* drv_wifi_stop_ap (void) {
    if (!WIFI_AP_ON) {
        return NULL;
    }
    ap_clients_gone();
    WIFI_AP_ON = false;
    WIFI_AP_SSID[0] = '\0';
    WIFI_AP_IP[0] = '\0';
    WIFI_AP_CH = 0;
    if (!WIFI_STA_ON && !ESPNOW_ON) {
        WIFI_ST = WST_STOPPED;
    }
    trace("wifi.stop_ap");
    radio_check();
    return NULL;
}

const char* drv_wifi_status (const char** state) {
    *state = host_wifi_state();
    return NULL;
}

const char* drv_wifi_get_mac (char mac[18]) {
    strcpy(mac, HOST_MAC);
    return NULL;
}

const char* drv_wifi_get_channel (int* channel) {
    if (!WIFI_INITED && !ESPNOW_ON) {
        return drv_errf("wifi.get_channel : failed to get wifi channel: ESP_ERR_WIFI_NOT_INIT");
    }
    *channel = CHANNEL;
    return NULL;
}

const char* drv_wifi_info (drv_wifi_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->state = host_wifi_state();
    info->ap_active = WIFI_AP_ON;
    strcpy(info->mac, HOST_MAC);
    strcpy(info->ap_mac, HOST_AP_MAC);
    if (WIFI_ST == WST_CONNECTED || WIFI_ST == WST_GOT_IP) {
        strcpy(info->ssid, WIFI_SSID);
        strcpy(info->bssid, WIFI_BSSID);
        info->channel = WIFI_CH;
        info->rssi = WIFI_RSSI;
        info->has_rssi = true;
    }
    if (WIFI_ST == WST_GOT_IP) {
        strcpy(info->ip, WIFI_IP);
        strcpy(info->netmask, WIFI_MASK);
        strcpy(info->gateway, WIFI_GW);
    }
    if (WIFI_AP_ON) {
        strcpy(info->ap_ssid, WIFI_AP_SSID);
        strcpy(info->ap_ip, WIFI_AP_IP);
        if (info->channel == 0) {
            info->channel = (WIFI_AP_CH > 0) ? WIFI_AP_CH : CHANNEL;
        }
    } else if (info->channel == 0 && (WIFI_STA_ON || ESPNOW_ON)) {
        info->channel = CHANNEL;
    }
    return NULL;
}

bool drv_wifi_next (drv_wifi_evt_t* evt) {
    if (Q_WIFI_N == 0) {
        return false;
    }
    *evt = Q_WIFI[Q_WIFI_HEAD];
    Q_WIFI_HEAD = (Q_WIFI_HEAD + 1) % WIFI_QUEUE_LEN;
    Q_WIFI_N--;
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

///////////////////////////////////////////////////////////////////////////////
// SLEEP AND RTC RETENTION MEMORY
///////////////////////////////////////////////////////////////////////////////

static bool valid_rtc_pin (int p) {
    return p==0 || p==2 || p==4 || (p>=12 && p<=15) ||
           (p>=25 && p<=27) || (p>=32 && p<=39);
}

const char* drv_sleep_validate (bool is_deep, const drv_sleep_cfg_t* cfg) {
    const char* fn = is_deep ? "sleep.deep" : "sleep.light";
    if (cfg == NULL) {
        return drv_errf("%s : missing sleep configuration", fn);
    }
    if (cfg->duration_us < 0) {
        return drv_errf("%s : duration must be positive", fn);
    }
    if (cfg->has_ext0) {
        if (!valid_rtc_pin(cfg->ext0_pin)) {
            return drv_errf("%s : ext0 pin %d is not a valid RTC GPIO", fn, cfg->ext0_pin);
        }
        if (cfg->ext0_level != 0 && cfg->ext0_level != 1) {
            return drv_errf("%s : ext0 level must be 0 or 1", fn);
        }
        if (cfg->ext0_pull != NULL &&
            strcmp(cfg->ext0_pull, "none") &&
            strcmp(cfg->ext0_pull, "up") &&
            strcmp(cfg->ext0_pull, "down")) {
            return drv_errf("%s : invalid ext0 pull '%s' (expected none, up, down)",
                            fn, cfg->ext0_pull);
        }
    }
    if (cfg->ext1_n > 0) {
        for (int i = 0; i < cfg->ext1_n; i++) {
            if (!valid_rtc_pin(cfg->ext1_pins[i])) {
                return drv_errf("%s : ext1 pin %d is not a valid RTC GPIO", fn, cfg->ext1_pins[i]);
            }
        }
        if (cfg->ext1_mode != NULL &&
            strcmp(cfg->ext1_mode, "any_high") &&
            strcmp(cfg->ext1_mode, "all_low")) {
            return drv_errf("%s : invalid ext1 mode '%s' (expected any_high or all_low)",
                            fn, cfg->ext1_mode);
        }
    }
    if (cfg->gpio_n > 0) {
        if (is_deep) {
            return drv_errf("%s : digital gpio wakeup is only supported in sleep.light "
                            "(use ext0 or ext1 for deep sleep)", fn);
        }
        for (int i = 0; i < cfg->gpio_n; i++) {
            if (!valid_pin(cfg->gpio[i].pin)) {
                return drv_errf("%s : invalid GPIO pin %d", fn, cfg->gpio[i].pin);
            }
            if (cfg->gpio[i].level != 0 && cfg->gpio[i].level != 1) {
                return drv_errf("%s : gpio wakeup level must be 0 or 1", fn);
            }
        }
    }
    if (cfg->isolate_n > 0) {
        if (!is_deep) {
            return drv_errf("%s : isolate is only supported in sleep.deep", fn);
        }
        for (int i = 0; i < cfg->isolate_n; i++) {
            if (!valid_rtc_pin(cfg->isolate[i])) {
                return drv_errf("%s : isolate pin %d is not a valid RTC GPIO", fn, cfg->isolate[i]);
            }
        }
    }
    bool has_wake = (cfg->duration_us > 0) || cfg->has_ext0 ||
                    (cfg->ext1_n > 0) || (cfg->gpio_n > 0);
    if (!has_wake && !(is_deep && cfg->force)) {
        return drv_errf("%s : no wakeup source configured "
                        "(pass duration, ext0, ext1%s)",
                        fn, is_deep ? ", or force=true" : ", or gpio");
    }
    return NULL;
}

const char* drv_sleep_deep (const drv_sleep_cfg_t* cfg) {
    script_read();
    const char* err = drv_sleep_validate(true, cfg);
    if (err != NULL) {
        return err;
    }
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "sleep.deep");
    if (cfg->duration_us > 0) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " duration=%lldms",
                      (long long)(cfg->duration_us / 1000));
    }
    if (cfg->has_ext0) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " ext0=%d:%d",
                      cfg->ext0_pin, cfg->ext0_level);
    }
    if (cfg->ext1_n > 0) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " ext1=%d(%s)",
                      cfg->ext1_n, cfg->ext1_mode ? cfg->ext1_mode : "any_high");
    }
    if (cfg->isolate_n > 0) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " isolate=%d", cfg->isolate_n);
    }
    if (cfg->force) {
        snprintf(buf + n, sizeof(buf) - (size_t)n, " force");
    }
    trace("%s", buf);
    QUIT = true;
    return NULL;
}

static bool pin_matches_light_wake (const drv_sleep_cfg_t* cfg, int pin, int level,
                                    const char** cause_out, int* ext1_pin_out) {
    for (int i = 0; i < cfg->gpio_n; i++) {
        if (cfg->gpio[i].pin == pin && cfg->gpio[i].level == level) {
            *cause_out = "gpio";
            return true;
        }
    }
    if (cfg->has_ext0 && cfg->ext0_pin == pin && cfg->ext0_level == level) {
        *cause_out = "ext0";
        return true;
    }
    if (cfg->ext1_n > 0) {
        bool all_low = (cfg->ext1_mode != NULL && !strcmp(cfg->ext1_mode, "all_low"));
        for (int i = 0; i < cfg->ext1_n; i++) {
            if (cfg->ext1_pins[i] == pin) {
                if (!all_low && level == 1) {
                    *cause_out = "ext1";
                    *ext1_pin_out = pin;
                    return true;
                }
                if (all_low && level == 0) {
                    bool ok = true;
                    for (int j = 0; j < cfg->ext1_n; j++) {
                        int lv = (cfg->ext1_pins[j] == pin) ? level : PINS[cfg->ext1_pins[j]].level;
                        if (lv != 0) ok = false;
                    }
                    if (ok) {
                        *cause_out = "ext1";
                        *ext1_pin_out = pin;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

const char* drv_sleep_light (const drv_sleep_cfg_t* cfg, drv_sleep_wake_t* wake) {
    script_read();
    const char* err = drv_sleep_validate(false, cfg);
    if (err != NULL) {
        return err;
    }
    trace("sleep.light duration=%lldms",
          (long long)(cfg->duration_us > 0 ? cfg->duration_us / 1000 : 0));

    int64_t t0 = NOW;
    int64_t deadline = (cfg->duration_us > 0) ? (t0 + cfg->duration_us) : -1;

    // Find the earliest pending pin event matching a configured wakeup source
    int best_idx = -1;
    const char* pin_cause = NULL;
    int ext1_pin = -1;
    for (int i = 0; i < NPEND; i++) {
        if (PEND[i].kind != P_PIN) continue;
        if (deadline >= 0 && PEND[i].at > deadline) continue;
        const char* c = NULL;
        int ep = -1;
        if (pin_matches_light_wake(cfg, PEND[i].a, PEND[i].b, &c, &ep)) {
            if (best_idx == -1 || PEND[i].at < PEND[best_idx].at ||
                (PEND[i].at == PEND[best_idx].at && PEND[i].seq < PEND[best_idx].seq)) {
                best_idx = i;
                pin_cause = c;
                ext1_pin = ep;
            }
        }
    }

    memset(wake, 0, sizeof(*wake));
    int64_t target = t0;
    if (best_idx != -1) {
        target = PEND[best_idx].at;
        wake->cause = pin_cause;
        wake->causes[0] = pin_cause;
        wake->causes_n = 1;
        if (ext1_pin >= 0) {
            wake->pins[0] = ext1_pin;
            wake->pins_n = 1;
        }
    } else if (deadline >= 0) {
        target = deadline;
        wake->cause = "timer";
        wake->causes[0] = "timer";
        wake->causes_n = 1;
    } else {
        wake->cause = "none";
    }

    if (target > NOW) {
        sleep_us(target - NOW);
        NOW = target;
    }
    // Fire any pending events up to wakeup time (updating pin levels)
    while (1) {
        int k = pending_next();
        if (k == -1 || PEND[k].at > NOW) break;
        pending_t p = PEND[k];
        PEND[k] = PEND[--NPEND];
        pending_fire(p);
    }
    wake->slept_us = NOW - t0;
    SLEEP_WAKE = *wake;
    return NULL;
}

void drv_sleep_info (drv_sleep_wake_t* wake) {
    script_read();
    *wake = SLEEP_WAKE;
}

const char* drv_sleep_reset_reason (void) {
    script_read();
    return SLEEP_RESET;
}

static const char* hrtc_check_key (const char* fn, const char* key) {
    size_t n = (key != NULL) ? strlen(key) : 0;
    if (n < 1 || n > 15) {
        return drv_errf("%s : key length must be between 1 and 15 characters", fn);
    }
    return NULL;
}

static host_rtc_slot_t* hrtc_find (const char* key) {
    script_read();
    for (int i = 0; i < HOST_RTC_SLOTS; i++) {
        if (HOST_RTC[i].used && !strcmp(HOST_RTC[i].key, key)) {
            return &HOST_RTC[i];
        }
    }
    return NULL;
}

const char* drv_rtc_put_int (const char* key, int64_t v) {
    script_read();
    const char* err = hrtc_check_key("sleep.rtc_put_int", key);
    if (err != NULL) return err;
    if (host_rtc_put_raw(key, HRTC_INT, v, NULL) != NULL) {
        return drv_errf("sleep.rtc_put_int : RTC memory store is full (max %d keys)", HOST_RTC_SLOTS);
    }
    trace("sleep.rtc_put_int %s %lld", key, (long long)v);
    return NULL;
}

const char* drv_rtc_get_int (const char* key, bool* found, int64_t* v) {
    const char* err = hrtc_check_key("sleep.rtc_get_int", key);
    if (err != NULL) return err;
    host_rtc_slot_t* sl = hrtc_find(key);
    if (sl == NULL) { *found = false; return NULL; }
    if (sl->type != HRTC_INT) {
        return drv_errf("sleep.rtc_get_int : key '%s' is not an int", key);
    }
    *found = true;
    *v = sl->i;
    return NULL;
}

const char* drv_rtc_put_bool (const char* key, bool v) {
    script_read();
    const char* err = hrtc_check_key("sleep.rtc_put_bool", key);
    if (err != NULL) return err;
    if (host_rtc_put_raw(key, HRTC_BOOL, v ? 1 : 0, NULL) != NULL) {
        return drv_errf("sleep.rtc_put_bool : RTC memory store is full (max %d keys)", HOST_RTC_SLOTS);
    }
    trace("sleep.rtc_put_bool %s %s", key, v ? "true" : "false");
    return NULL;
}

const char* drv_rtc_get_bool (const char* key, bool* found, bool* v) {
    const char* err = hrtc_check_key("sleep.rtc_get_bool", key);
    if (err != NULL) return err;
    host_rtc_slot_t* sl = hrtc_find(key);
    if (sl == NULL) { *found = false; return NULL; }
    if (sl->type != HRTC_BOOL) {
        return drv_errf("sleep.rtc_get_bool : key '%s' is not a bool", key);
    }
    *found = true;
    *v = (sl->i != 0);
    return NULL;
}

const char* drv_rtc_put_str (const char* key, const char* v) {
    script_read();
    const char* err = hrtc_check_key("sleep.rtc_put_string", key);
    if (err != NULL) return err;
    size_t len = (v != NULL) ? strlen(v) : 0;
    if (len >= DRV_RTC_STR_MAX) {
        return drv_errf("sleep.rtc_put_string : string length %d exceeds max %d",
                        (int)len, DRV_RTC_STR_MAX - 1);
    }
    if (host_rtc_put_raw(key, HRTC_STR, 0, v) != NULL) {
        return drv_errf("sleep.rtc_put_string : RTC memory store is full (max %d keys)", HOST_RTC_SLOTS);
    }
    trace("sleep.rtc_put_string %s %s", key, v);
    return NULL;
}

const char* drv_rtc_get_str (const char* key, bool* found, const char** v) {
    const char* err = hrtc_check_key("sleep.rtc_get_string", key);
    if (err != NULL) return err;
    host_rtc_slot_t* sl = hrtc_find(key);
    if (sl == NULL) { *found = false; return NULL; }
    if (sl->type != HRTC_STR) {
        return drv_errf("sleep.rtc_get_string : key '%s' is not a string", key);
    }
    *found = true;
    *v = sl->s;
    return NULL;
}

const char* drv_rtc_is_key (const char* key, bool* yes) {
    const char* err = hrtc_check_key("sleep.rtc_is_key", key);
    if (err != NULL) return err;
    *yes = (hrtc_find(key) != NULL);
    return NULL;
}

const char* drv_rtc_remove (const char* key) {
    const char* err = hrtc_check_key("sleep.rtc_remove", key);
    if (err != NULL) return err;
    host_rtc_slot_t* sl = hrtc_find(key);
    if (sl != NULL) {
        memset(sl, 0, sizeof(*sl));
        trace("sleep.rtc_remove %s", key);
    }
    return NULL;
}

void drv_rtc_clear (void) {
    script_read();
    memset(HOST_RTC, 0, sizeof(HOST_RTC));
    trace("sleep.rtc_clear");
}
