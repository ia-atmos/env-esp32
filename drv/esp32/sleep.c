// Sleep (deep sleep, light sleep, wakeup/reset inspection) and RTC slow-memory
// key-value storage on the ESP32: drv/drv.h over ESP-IDF v6.1.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drv.h"

///////////////////////////////////////////////////////////////////////////////
// HELPERS
///////////////////////////////////////////////////////////////////////////////

static const char* fail (const char* fn, const char* what, esp_err_t err) {
    return drv_errf("%s : %s: %s", fn, what, esp_err_to_name(err));
}

static const char* cause_name (esp_sleep_source_t src) {
    switch (src) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
        case ESP_SLEEP_WAKEUP_EXT0:      return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:      return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "touch";
        case ESP_SLEEP_WAKEUP_ULP:       return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:      return "gpio";
        case ESP_SLEEP_WAKEUP_UART0:
        case ESP_SLEEP_WAKEUP_UART1:     return "uart";
        case ESP_SLEEP_WAKEUP_WIFI:      return "wifi";
        case ESP_SLEEP_WAKEUP_BT:        return "bt";
        default:                         return "other";
    }
}

static void fill_wake_info (drv_sleep_wake_t* w) {
    memset(w, 0, sizeof(*w));
    uint32_t mask = esp_sleep_get_wakeup_causes();
    static const esp_sleep_source_t order[] = {
        ESP_SLEEP_WAKEUP_TIMER,
        ESP_SLEEP_WAKEUP_EXT0,
        ESP_SLEEP_WAKEUP_EXT1,
        ESP_SLEEP_WAKEUP_GPIO,
        ESP_SLEEP_WAKEUP_TOUCHPAD,
        ESP_SLEEP_WAKEUP_ULP,
        ESP_SLEEP_WAKEUP_UART0,
        ESP_SLEEP_WAKEUP_UART1,
        ESP_SLEEP_WAKEUP_WIFI,
        ESP_SLEEP_WAKEUP_BT,
    };
    for (size_t i = 0; i < sizeof(order)/sizeof(order[0]); i++) {
        if ((mask & (1U << order[i])) && w->causes_n < DRV_SLEEP_MAX_CAUSES) {
            const char* name = cause_name(order[i]);
            bool dup = false;
            for (int j = 0; j < w->causes_n; j++) {
                if (!strcmp(w->causes[j], name)) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                w->causes[w->causes_n++] = name;
            }
        }
    }
    w->cause = (w->causes_n > 0) ? w->causes[0] : "none";

#if SOC_PM_SUPPORT_EXT1_WAKEUP
    if (mask & (1U << ESP_SLEEP_WAKEUP_EXT1)) {
        uint64_t status = esp_sleep_get_ext1_wakeup_status();
        for (int p = 0; p < 64 && w->pins_n < DRV_SLEEP_MAX_PINS; p++) {
            if (status & (1ULL << p)) {
                w->pins[w->pins_n++] = p;
            }
        }
    }
#endif
}

///////////////////////////////////////////////////////////////////////////////
// VALIDATION & CONFIGURATION
///////////////////////////////////////////////////////////////////////////////

const char* drv_sleep_validate (bool is_deep, const drv_sleep_cfg_t* cfg) {
    const char* fn = is_deep ? "sleep.deep" : "sleep.light";
    if (cfg == NULL) {
        return drv_errf("%s : missing sleep configuration", fn);
    }
    if (cfg->duration_us < 0) {
        return drv_errf("%s : duration must be positive", fn);
    }
    if (cfg->has_ext0) {
        if (!GPIO_IS_VALID_GPIO(cfg->ext0_pin) ||
            !rtc_gpio_is_valid_gpio((gpio_num_t)cfg->ext0_pin)) {
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
            int p = cfg->ext1_pins[i];
            if (!GPIO_IS_VALID_GPIO(p) || !rtc_gpio_is_valid_gpio((gpio_num_t)p)) {
                return drv_errf("%s : ext1 pin %d is not a valid RTC GPIO", fn, p);
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
            if (!GPIO_IS_VALID_GPIO(cfg->gpio[i].pin)) {
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
            int p = cfg->isolate[i];
            if (!GPIO_IS_VALID_GPIO(p) || !rtc_gpio_is_valid_gpio((gpio_num_t)p)) {
                return drv_errf("%s : isolate pin %d is not a valid RTC GPIO", fn, p);
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

static const char* apply_common_wakeups (const char* fn, const drv_sleep_cfg_t* cfg) {
    (void) esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (cfg->duration_us > 0) {
        esp_err_t err = esp_sleep_enable_timer_wakeup((uint64_t)cfg->duration_us);
        if (err != ESP_OK) {
            return fail(fn, "esp_sleep_enable_timer_wakeup failed", err);
        }
    }

#if SOC_PM_SUPPORT_EXT0_WAKEUP
    if (cfg->has_ext0) {
        gpio_num_t p = (gpio_num_t) cfg->ext0_pin;
        (void) rtc_gpio_init(p);
        (void) rtc_gpio_set_direction(p, RTC_GPIO_MODE_INPUT_ONLY);
        if (cfg->ext0_pull != NULL && !strcmp(cfg->ext0_pull, "up")) {
            (void) rtc_gpio_pulldown_dis(p);
            (void) rtc_gpio_pullup_en(p);
        } else if (cfg->ext0_pull != NULL && !strcmp(cfg->ext0_pull, "down")) {
            (void) rtc_gpio_pullup_dis(p);
            (void) rtc_gpio_pulldown_en(p);
        } else {
            (void) rtc_gpio_pullup_dis(p);
            (void) rtc_gpio_pulldown_dis(p);
        }
        esp_err_t err = esp_sleep_enable_ext0_wakeup(p, cfg->ext0_level);
        if (err != ESP_OK) {
            return fail(fn, "esp_sleep_enable_ext0_wakeup failed", err);
        }
    }
#endif

#if SOC_PM_SUPPORT_EXT1_WAKEUP
    if (cfg->ext1_n > 0) {
        uint64_t mask = 0;
        for (int i = 0; i < cfg->ext1_n; i++) {
            mask |= (1ULL << cfg->ext1_pins[i]);
        }
        esp_sleep_ext1_wakeup_mode_t mode = ESP_EXT1_WAKEUP_ANY_HIGH;
        if (cfg->ext1_mode != NULL && !strcmp(cfg->ext1_mode, "all_low")) {
#if CONFIG_IDF_TARGET_ESP32
            mode = ESP_EXT1_WAKEUP_ALL_LOW;
#else
            mode = ESP_EXT1_WAKEUP_ANY_LOW;
#endif
        }
        esp_err_t err = esp_sleep_enable_ext1_wakeup_io(mask, mode);
        if (err != ESP_OK) {
            return fail(fn, "esp_sleep_enable_ext1_wakeup_io failed", err);
        }
    }
#endif

    return NULL;
}

const char* drv_sleep_deep (const drv_sleep_cfg_t* cfg) {
    const char* err_msg = drv_sleep_validate(true, cfg);
    if (err_msg != NULL) {
        return err_msg;
    }
    err_msg = apply_common_wakeups("sleep.deep", cfg);
    if (err_msg != NULL) {
        return err_msg;
    }
    for (int i = 0; i < cfg->isolate_n; i++) {
        (void) rtc_gpio_isolate((gpio_num_t) cfg->isolate[i]);
    }
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_deep_sleep_start();
    return NULL;
}

const char* drv_sleep_light (const drv_sleep_cfg_t* cfg, drv_sleep_wake_t* wake) {
    const char* err_msg = drv_sleep_validate(false, cfg);
    if (err_msg != NULL) {
        return err_msg;
    }
    err_msg = apply_common_wakeups("sleep.light", cfg);
    if (err_msg != NULL) {
        return err_msg;
    }
    if (cfg->gpio_n > 0) {
        for (int i = 0; i < cfg->gpio_n; i++) {
            gpio_num_t p = (gpio_num_t) cfg->gpio[i].pin;
            gpio_int_type_t lvl = cfg->gpio[i].level
                                ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
            esp_err_t err = gpio_wakeup_enable(p, lvl);
            if (err != ESP_OK) {
                for (int j = 0; j < i; j++) {
                    (void) gpio_wakeup_disable((gpio_num_t) cfg->gpio[j].pin);
                }
                return fail("sleep.light", "gpio_wakeup_enable failed", err);
            }
        }
        esp_err_t err = esp_sleep_enable_gpio_wakeup();
        if (err != ESP_OK) {
            for (int i = 0; i < cfg->gpio_n; i++) {
                (void) gpio_wakeup_disable((gpio_num_t) cfg->gpio[i].pin);
            }
            return fail("sleep.light", "esp_sleep_enable_gpio_wakeup failed", err);
        }
    }

    fflush(stdout);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_light_sleep_start();
    int64_t slept = esp_timer_get_time() - t0;

    for (int i = 0; i < cfg->gpio_n; i++) {
        (void) gpio_wakeup_disable((gpio_num_t) cfg->gpio[i].pin);
    }
    (void) esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (err != ESP_OK) {
        return fail("sleep.light", "esp_light_sleep_start failed", err);
    }
    fill_wake_info(wake);
    wake->slept_us = (slept > 0) ? slept : 0;
    return NULL;
}

void drv_sleep_info (drv_sleep_wake_t* wake) {
    fill_wake_info(wake);
}

const char* drv_sleep_reset_reason (void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:    return "poweron";
        case ESP_RST_EXT:        return "ext";
        case ESP_RST_SW:         return "sw";
        case ESP_RST_PANIC:      return "panic";
        case ESP_RST_INT_WDT:    return "int_wdt";
        case ESP_RST_TASK_WDT:   return "task_wdt";
        case ESP_RST_WDT:        return "wdt";
        case ESP_RST_DEEPSLEEP:  return "deepsleep";
        case ESP_RST_BROWNOUT:   return "brownout";
        case ESP_RST_SDIO:       return "sdio";
        case ESP_RST_USB:        return "usb";
        case ESP_RST_JTAG:       return "jtag";
        case ESP_RST_EFUSE:      return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default:                 return "unknown";
    }
}

///////////////////////////////////////////////////////////////////////////////
// RTC RETENTION MEMORY (RTC_DATA_ATTR: survives deep sleep, zero flash wear)
///////////////////////////////////////////////////////////////////////////////

#define RTC_MAGIC   0x52544331U
#define RTC_SLOTS   32

typedef enum {
    RTC_T_NONE = 0,
    RTC_T_INT,
    RTC_T_BOOL,
    RTC_T_STR,
} rtc_type_t;

typedef struct {
    uint8_t used;
    uint8_t type;
    char    key[16];
    int64_t i;
    char    s[DRV_RTC_STR_MAX];
} rtc_slot_t;

typedef struct {
    uint32_t   magic;
    uint32_t   hash;
    rtc_slot_t slots[RTC_SLOTS];
} rtc_store_t;

static RTC_DATA_ATTR rtc_store_t s_rtc;
static bool s_rtc_checked = false;

static uint32_t rtc_hash (const rtc_slot_t slots[RTC_SLOTS]) {
    const uint8_t* p = (const uint8_t*) slots;
    size_t n = sizeof(rtc_slot_t) * RTC_SLOTS;
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

static void rtc_seal (void) {
    s_rtc.magic = RTC_MAGIC;
    s_rtc.hash = rtc_hash(s_rtc.slots);
}

static void rtc_ensure (void) {
    if (s_rtc_checked) {
        return;
    }
    s_rtc_checked = true;
    if (s_rtc.magic != RTC_MAGIC || s_rtc.hash != rtc_hash(s_rtc.slots)) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        rtc_seal();
    }
}

static const char* rtc_check_key (const char* fn, const char* key) {
    size_t n = (key != NULL) ? strlen(key) : 0;
    if (n < 1 || n > 15) {
        return drv_errf("%s : key length must be between 1 and 15 characters", fn);
    }
    return NULL;
}

static rtc_slot_t* rtc_find (const char* key) {
    rtc_ensure();
    for (int i = 0; i < RTC_SLOTS; i++) {
        if (s_rtc.slots[i].used && !strcmp(s_rtc.slots[i].key, key)) {
            return &s_rtc.slots[i];
        }
    }
    return NULL;
}

static rtc_slot_t* rtc_alloc (const char* fn, const char* key, const char** err) {
    rtc_slot_t* sl = rtc_find(key);
    if (sl != NULL) {
        return sl;
    }
    for (int i = 0; i < RTC_SLOTS; i++) {
        if (!s_rtc.slots[i].used) {
            sl = &s_rtc.slots[i];
            memset(sl, 0, sizeof(*sl));
            sl->used = 1;
            strcpy(sl->key, key);
            return sl;
        }
    }
    *err = drv_errf("%s : RTC memory store is full (max %d keys)", fn, RTC_SLOTS);
    return NULL;
}

const char* drv_rtc_put_int (const char* key, int64_t v) {
    const char* err = rtc_check_key("sleep.rtc_put_int", key);
    if (err != NULL) return err;
    rtc_slot_t* sl = rtc_alloc("sleep.rtc_put_int", key, &err);
    if (sl == NULL) return err;
    sl->type = RTC_T_INT;
    sl->i = v;
    sl->s[0] = '\0';
    rtc_seal();
    return NULL;
}

const char* drv_rtc_get_int (const char* key, bool* found, int64_t* v) {
    const char* err = rtc_check_key("sleep.rtc_get_int", key);
    if (err != NULL) return err;
    rtc_slot_t* sl = rtc_find(key);
    if (sl == NULL) {
        *found = false;
        return NULL;
    }
    if (sl->type != RTC_T_INT) {
        return drv_errf("sleep.rtc_get_int : key '%s' is not an int", key);
    }
    *found = true;
    *v = sl->i;
    return NULL;
}

const char* drv_rtc_put_bool (const char* key, bool v) {
    const char* err = rtc_check_key("sleep.rtc_put_bool", key);
    if (err != NULL) return err;
    rtc_slot_t* sl = rtc_alloc("sleep.rtc_put_bool", key, &err);
    if (sl == NULL) return err;
    sl->type = RTC_T_BOOL;
    sl->i = v ? 1 : 0;
    sl->s[0] = '\0';
    rtc_seal();
    return NULL;
}

const char* drv_rtc_get_bool (const char* key, bool* found, bool* v) {
    const char* err = rtc_check_key("sleep.rtc_get_bool", key);
    if (err != NULL) return err;
    rtc_slot_t* sl = rtc_find(key);
    if (sl == NULL) {
        *found = false;
        return NULL;
    }
    if (sl->type != RTC_T_BOOL) {
        return drv_errf("sleep.rtc_get_bool : key '%s' is not a bool", key);
    }
    *found = true;
    *v = (sl->i != 0);
    return NULL;
}

const char* drv_rtc_put_str (const char* key, const char* v) {
    const char* err = rtc_check_key("sleep.rtc_put_string", key);
    if (err != NULL) return err;
    size_t len = (v != NULL) ? strlen(v) : 0;
    if (len >= DRV_RTC_STR_MAX) {
        return drv_errf("sleep.rtc_put_string : string length %d exceeds max %d",
                        (int)len, DRV_RTC_STR_MAX - 1);
    }
    rtc_slot_t* sl = rtc_alloc("sleep.rtc_put_string", key, &err);
    if (sl == NULL) return err;
    sl->type = RTC_T_STR;
    sl->i = 0;
    if (len > 0) {
        memcpy(sl->s, v, len);
    }
    sl->s[len] = '\0';
    rtc_seal();
    return NULL;
}

const char* drv_rtc_get_str (const char* key, bool* found, const char** v) {
    const char* err = rtc_check_key("sleep.rtc_get_string", key);
    if (err != NULL) return err;
    rtc_slot_t* sl = rtc_find(key);
    if (sl == NULL) {
        *found = false;
        return NULL;
    }
    if (sl->type != RTC_T_STR) {
        return drv_errf("sleep.rtc_get_string : key '%s' is not a string", key);
    }
    *found = true;
    *v = sl->s;
    return NULL;
}

const char* drv_rtc_is_key (const char* key, bool* yes) {
    const char* err = rtc_check_key("sleep.rtc_is_key", key);
    if (err != NULL) return err;
    *yes = (rtc_find(key) != NULL);
    return NULL;
}

const char* drv_rtc_remove (const char* key) {
    const char* err = rtc_check_key("sleep.rtc_remove", key);
    if (err != NULL) return err;
    rtc_slot_t* sl = rtc_find(key);
    if (sl != NULL) {
        memset(sl, 0, sizeof(*sl));
        rtc_seal();
    }
    return NULL;
}

void drv_rtc_clear (void) {
    s_rtc_checked = true;
    memset(&s_rtc, 0, sizeof(s_rtc));
    rtc_seal();
}
