// The drivers of the board, in plain C: no Lua and no Atmos.
//
// Two implementations share this interface:
//   - drv/esp32/: ESP-IDF (GPIO, LEDC, ADC, RMT), for the board;
//   - drv/host/:  a simulation for the host, with a virtual clock and
//                 input from a script (see drv/host/host.c).
//
// The environments in env/*.atm call these functions from native
// literals. A function that can fail returns NULL on success, or a
// message (a static buffer, valid until the next call).
//
// Interrupts and driver tasks never call into Atmos: they queue events,
// which the environments drain in their `step`, as `drv_*_next`.

#ifndef DRV_H
#define DRV_H

#include <stdbool.h>
#include <stdint.h>

///////////////////////////////////////////////////////////////////////////////
// EVENTS
///////////////////////////////////////////////////////////////////////////////

typedef enum {
    DRV_EVT_NONE = 0,
    DRV_EVT_GPIO,           // a watched pin changed
    DRV_EVT_PWM_FADE,       // a fade of a PWM channel completed
    DRV_EVT_IR,             // an IR frame was decoded
} drv_evt_kind_t;

typedef struct {
    drv_evt_kind_t kind;
    int64_t time_us;        // since boot
    union {
        struct {
            int pin;
            int value;      // 0 or 1
        } gpio;
        struct {
            int channel;
        } fade;
        struct {
            const char* protocol;   // "nec", "samsung", "sony", "lg"
            uint32_t address;
            uint32_t command;
            uint64_t raw;
            int bits;
            bool repeat;
        } ir;
    } u;
} drv_evt_t;

///////////////////////////////////////////////////////////////////////////////
// CLOCK
///////////////////////////////////////////////////////////////////////////////

// Microseconds since boot.
int64_t drv_clock_now (void);

// Waits until `max_us` microseconds pass, or until a driver queues an
// event, whichever comes first; `max_us < 0` waits only for an event.
// Gives the microseconds that passed, or -1 when nothing can happen
// anymore (only the host, at the end of its script).
int64_t drv_clock_wait (int64_t max_us);

// Ends a `drv_clock_wait` in course: called by the drivers when they
// queue an event (also from an interrupt).
void drv_clock_wake (void);

///////////////////////////////////////////////////////////////////////////////
// GPIO
///////////////////////////////////////////////////////////////////////////////

// mode: "input", "input_pullup", "input_pulldown", "output", "open_drain"
const char* drv_gpio_setup   (int pin, const char* mode);
const char* drv_gpio_read    (int pin, int* level);
const char* drv_gpio_write   (int pin, int level);

// edge: "rising", "falling", "both" or "change"
const char* drv_gpio_watch   (int pin, const char* edge);
const char* drv_gpio_unwatch (int pin);

// atten: "0db", "2.5db", "6db", "11db", "12db", or NULL (12db)
const char* drv_gpio_read_adc (int pin, const char* atten, int* raw);

// PWM (LEDC): a channel is set up, then pins attach to it.
const char* drv_pwm_setup  (int channel, int freq, int resolution);
const char* drv_pwm_attach (int pin, int channel);
const char* drv_pwm_detach (int pin);
const char* drv_pwm_stop   (int channel);
const char* drv_pwm_write  (int channel, int duty);
const char* drv_pwm_fade   (int channel, int duty, int duration_ms);

// Pin changes and completed fades, in order.
bool drv_gpio_next (drv_evt_t* evt);

///////////////////////////////////////////////////////////////////////////////
// IR RECEIVER
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    uint32_t captured;
    uint32_t decoded;
    uint32_t invalid;
    uint32_t dropped;
} drv_ir_stats_t;

// glitch_ns, idle_ns: negative for the defaults (1250 ns, 15 ms)
const char* drv_ir_start (int pin, int64_t glitch_ns, int64_t idle_ns);
const char* drv_ir_stop  (void);
void        drv_ir_stats (drv_ir_stats_t* stats);
bool        drv_ir_next  (drv_evt_t* evt);

///////////////////////////////////////////////////////////////////////////////
// ESP-NOW
///////////////////////////////////////////////////////////////////////////////

#define DRV_ESPNOW_MAX_DATA 250     // ESP_NOW_MAX_DATA_LEN

typedef enum {
    DRV_ESPNOW_NONE = 0,
    DRV_ESPNOW_RECV,                // a message arrived
    DRV_ESPNOW_SEND,                // the result of a send
} drv_espnow_kind_t;

// Apart from drv_evt_t, which would grow by the payload.
typedef struct {
    drv_espnow_kind_t kind;
    int64_t time_us;
    char mac[18];                   // "24:0a:c4:05:06:07"
    bool ok;                        // SEND: delivered (unicast) or sent
    int len;                        // RECV
    uint8_t data[DRV_ESPNOW_MAX_DATA];
} drv_espnow_evt_t;

// Wi-Fi in station mode, channel 1, with the broadcast peer.
const char* drv_espnow_init        (void);
const char* drv_espnow_add_peer    (const char* mac);
const char* drv_espnow_send        (const char* mac, const uint8_t* data, int len);
const char* drv_espnow_get_mac     (char mac[18]);
const char* drv_espnow_get_channel (int* channel);
const char* drv_espnow_set_channel (int channel);
bool        drv_espnow_next        (drv_espnow_evt_t* evt);

///////////////////////////////////////////////////////////////////////////////
// PREFERENCES (NVS): one namespace open at a time
///////////////////////////////////////////////////////////////////////////////

// The namespace and the keys have 1 to 15 characters.
const char* drv_prefs_open   (const char* ns, bool readonly, const char* partition);
const char* drv_prefs_close  (void);
const char* drv_prefs_commit (void);

// A put commits at once if `commit`. A get sets `*found` to false if
// the key is absent.
const char* drv_prefs_put_int  (const char* key, int32_t v, bool commit);
const char* drv_prefs_get_int  (const char* key, bool* found, int32_t* v);
const char* drv_prefs_put_bool (const char* key, bool v, bool commit);
const char* drv_prefs_get_bool (const char* key, bool* found, bool* v);
const char* drv_prefs_put_str  (const char* key, const char* v, bool commit);
// `*v` is valid until the next call
const char* drv_prefs_get_str  (const char* key, bool* found, const char** v);
const char* drv_prefs_is_key   (const char* key, bool* yes);
const char* drv_prefs_remove   (const char* key, bool commit);
const char* drv_prefs_clear    (bool commit);

///////////////////////////////////////////////////////////////////////////////
// MEMORY
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    int64_t heap_total;
    int64_t heap_free;
    int64_t heap_min_free;
    int64_t heap_largest_free;
    int64_t internal_total;
    int64_t internal_free;
    int64_t internal_min_free;
    int64_t internal_largest_free;
    int64_t stack_free;
} drv_meminfo_t;

void drv_meminfo (drv_meminfo_t* m);

///////////////////////////////////////////////////////////////////////////////
// ERRORS (drv/err.c)
///////////////////////////////////////////////////////////////////////////////

// A message in a static buffer, as `printf`.
const char* drv_errf (const char* fmt, ...);

#endif
