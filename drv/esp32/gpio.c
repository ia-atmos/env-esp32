// GPIO, ADC and PWM (LEDC) on the ESP32: drv/drv.h over ESP-IDF.
// From main/gpio.c (the Lua version), with the Lua API taken out.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"

#include "drv.h"

#define EVENT_QUEUE_LEN 32
#ifndef SOC_GPIO_PIN_COUNT
#define SOC_GPIO_PIN_COUNT 48
#endif
#define GPIO_MAX_TRACKED SOC_GPIO_PIN_COUNT
#define MAX_ADC_CHANNELS 10

static QueueHandle_t s_queue = NULL;
static bool s_isr_attached[GPIO_MAX_TRACKED];
static bool s_fade_installed = false;

static adc_oneshot_unit_handle_t s_adc1 = NULL;
static adc_oneshot_unit_handle_t s_adc2 = NULL;
static int8_t s_adc1_atten[MAX_ADC_CHANNELS] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
static int8_t s_adc2_atten[MAX_ADC_CHANNELS] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};

typedef struct {
    bool configured;
    bool initialized;
    int freq;
    int resolution;
} pwm_channel_t;

static pwm_channel_t s_pwm[LEDC_CHANNEL_MAX];
static int  s_pin_pwm[GPIO_MAX_TRACKED];
static bool s_inited = false;

static void init (void) {
    if (s_inited) {
        return;
    }
    s_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(drv_evt_t));
    for (int i=0; i<LEDC_CHANNEL_MAX; i++) {
        s_pwm[i] = (pwm_channel_t) { 0 };
    }
    for (int i=0; i<GPIO_MAX_TRACKED; i++) {
        s_pin_pwm[i] = -1;
    }
    s_inited = true;
}

static void detach (int pin) {
    if (pin<0 || pin>=GPIO_MAX_TRACKED || s_pin_pwm[pin]==-1) {
        return;
    }
    (void) gpio_output_disable((gpio_num_t)pin);
    (void) gpio_reset_pin((gpio_num_t)pin);
    s_pin_pwm[pin] = -1;
}

static bool valid_pin (int pin) {
    return GPIO_IS_VALID_GPIO(pin) && pin<GPIO_MAX_TRACKED;
}

static bool valid_output_pin (int pin) {
    return GPIO_IS_VALID_OUTPUT_GPIO(pin) && pin<GPIO_MAX_TRACKED;
}

static const char* fail (const char* fn, const char* what, esp_err_t err) {
    return drv_errf("%s : %s failed: %s", fn, what, esp_err_to_name(err));
}

static const char* bad_channel (const char* fn, int ch) {
    if (ch<0 || ch>=LEDC_CHANNEL_MAX) {
        return drv_errf("%s : invalid LEDC channel %d", fn, ch);
    }
    if (!s_pwm[ch].configured) {
        return drv_errf("%s : PWM channel %d has not been configured "
                        "(call setup_pwm first)", fn, ch);
    }
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// INTERRUPTS: queue events and wake the clock
///////////////////////////////////////////////////////////////////////////////

static void IRAM_ATTR gpio_isr (void* arg) {
    int pin = (int)(intptr_t)arg;
    if (s_queue == NULL) {
        return;
    }
    drv_evt_t e = { .kind=DRV_EVT_GPIO, .time_us=esp_timer_get_time() };
    e.u.gpio.pin   = pin;
    e.u.gpio.value = gpio_get_level((gpio_num_t)pin);
    BaseType_t woken = pdFALSE;
    (void) xQueueSendFromISR(s_queue, &e, &woken);
    drv_clock_wake();
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static IRAM_ATTR bool fade_cb (const ledc_cb_param_t* param, void* arg) {
    (void) arg;
    if (s_queue == NULL) {
        return false;
    }
    drv_evt_t e = { .kind=DRV_EVT_PWM_FADE, .time_us=esp_timer_get_time() };
    e.u.fade.channel = param ? (int)param->channel : 0;
    BaseType_t woken = pdFALSE;
    (void) xQueueSendFromISR(s_queue, &e, &woken);
    drv_clock_wake();
    return woken == pdTRUE;
}

bool drv_gpio_next (drv_evt_t* evt) {
    return s_queue!=NULL && xQueueReceive(s_queue, evt, 0)==pdTRUE;
}

///////////////////////////////////////////////////////////////////////////////
// GPIO
///////////////////////////////////////////////////////////////////////////////

const char* drv_gpio_setup (int pin, const char* mode) {
    init();
    if (!valid_pin(pin)) {
        return drv_errf("gpio.setup : invalid GPIO pin %d", pin);
    }
    if (s_pin_pwm[pin] != -1) {
        detach(pin);
    }
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (!strcmp(mode, "input")) {
        conf.mode = GPIO_MODE_INPUT;
    } else if (!strcmp(mode, "input_pullup")) {
        conf.mode = GPIO_MODE_INPUT;
        conf.pull_up_en = GPIO_PULLUP_ENABLE;
    } else if (!strcmp(mode, "input_pulldown")) {
        conf.mode = GPIO_MODE_INPUT;
        conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else if (!strcmp(mode, "output") || !strcmp(mode, "open_drain")) {
        if (!valid_output_pin(pin)) {
            return drv_errf("gpio.setup : GPIO pin %d cannot be used as output", pin);
        }
        conf.mode = !strcmp(mode,"output") ? GPIO_MODE_OUTPUT : GPIO_MODE_OUTPUT_OD;
    } else if (!strcmp(mode, "pwm")) {
        return drv_errf("gpio.setup : invalid gpio mode 'pwm'; "
                        "use gpio.setup_pwm and gpio.attach_pwm instead");
    } else {
        return drv_errf("gpio.setup : invalid gpio mode '%s'", mode);
    }
    esp_err_t err = gpio_config(&conf);
    return (err == ESP_OK) ? NULL : fail("gpio.setup", "gpio_config", err);
}

const char* drv_gpio_read (int pin, int* level) {
    if (!valid_pin(pin)) {
        return drv_errf("gpio.read : invalid GPIO pin %d", pin);
    }
    *level = gpio_get_level((gpio_num_t)pin);
    return NULL;
}

const char* drv_gpio_write (int pin, int level) {
    if (!valid_output_pin(pin)) {
        return drv_errf("gpio.write : GPIO pin %d cannot be used as output", pin);
    }
    esp_err_t err = gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
    return (err == ESP_OK) ? NULL : fail("gpio.write", "gpio_set_level", err);
}

const char* drv_gpio_watch (int pin, const char* edge) {
    init();
    if (!valid_pin(pin)) {
        return drv_errf("gpio.watch : invalid GPIO pin %d", pin);
    }
    gpio_int_type_t t;
    if (!strcmp(edge, "rising")) {
        t = GPIO_INTR_POSEDGE;
    } else if (!strcmp(edge, "falling")) {
        t = GPIO_INTR_NEGEDGE;
    } else if (!strcmp(edge, "both") || !strcmp(edge, "change")) {
        t = GPIO_INTR_ANYEDGE;
    } else {
        return drv_errf("gpio.watch : invalid edge '%s'", edge);
    }
    if (s_queue == NULL) {
        return drv_errf("gpio.watch : failed to initialize the event queue");
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) {
        return fail("gpio.watch", "gpio_install_isr_service", err);
    }
    err = gpio_set_intr_type((gpio_num_t)pin, t);
    if (err != ESP_OK) {
        return fail("gpio.watch", "gpio_set_intr_type", err);
    }
    if (!s_isr_attached[pin]) {
        err = gpio_isr_handler_add((gpio_num_t)pin, gpio_isr, (void*)(intptr_t)pin);
        if (err != ESP_OK) {
            return fail("gpio.watch", "gpio_isr_handler_add", err);
        }
        s_isr_attached[pin] = true;
    }
    err = gpio_intr_enable((gpio_num_t)pin);
    return (err == ESP_OK) ? NULL : fail("gpio.watch", "gpio_intr_enable", err);
}

const char* drv_gpio_unwatch (int pin) {
    if (!valid_pin(pin)) {
        return drv_errf("gpio.unwatch : invalid GPIO pin %d", pin);
    }
    esp_err_t err = gpio_intr_disable((gpio_num_t)pin);
    if (err != ESP_OK) {
        return fail("gpio.unwatch", "gpio_intr_disable", err);
    }
    if (s_isr_attached[pin]) {
        err = gpio_isr_handler_remove((gpio_num_t)pin);
        if (err != ESP_OK) {
            return fail("gpio.unwatch", "gpio_isr_handler_remove", err);
        }
        s_isr_attached[pin] = false;
    }
    err = gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    return (err == ESP_OK) ? NULL : fail("gpio.unwatch", "gpio_set_intr_type", err);
}

///////////////////////////////////////////////////////////////////////////////
// ADC
///////////////////////////////////////////////////////////////////////////////

const char* drv_gpio_read_adc (int pin, const char* atten, int* raw) {
    adc_atten_t a;
    if (atten==NULL || !strcmp(atten,"11db") || !strcmp(atten,"12db")) {
        a = ADC_ATTEN_DB_12;
    } else if (!strcmp(atten, "0db")) {
        a = ADC_ATTEN_DB_0;
    } else if (!strcmp(atten, "2.5db")) {
        a = ADC_ATTEN_DB_2_5;
    } else if (!strcmp(atten, "6db")) {
        a = ADC_ATTEN_DB_6;
    } else {
        return drv_errf("gpio.read_adc : invalid attenuation "
                        "(expected 0db, 2.5db, 6db, 11db, 12db)");
    }

    adc_unit_t unit;
    adc_channel_t ch;
    esp_err_t err = adc_oneshot_io_to_channel(pin, &unit, &ch);
    if (err != ESP_OK) {
        return drv_errf("gpio.read_adc : GPIO pin %d is not supported by ADC: %s",
                        pin, esp_err_to_name(err));
    }

    adc_oneshot_unit_handle_t* h;
    int8_t* cache;
    if (unit == ADC_UNIT_1) {
        h = &s_adc1;
        cache = s_adc1_atten;
    } else if (unit == ADC_UNIT_2) {
        h = &s_adc2;
        cache = s_adc2_atten;
    } else {
        return drv_errf("gpio.read_adc : unknown ADC unit %d", unit);
    }
    if (*h == NULL) {
        adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id  = unit,
            .clk_src  = 0,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        err = adc_oneshot_new_unit(&cfg, h);
        if (err != ESP_OK) {
            return fail("gpio.read_adc", "adc_oneshot_new_unit", err);
        }
    }
    bool cached = (ch>=0 && ch<MAX_ADC_CHANNELS);
    if (!cached || cache[ch]!=(int8_t)a) {
        adc_oneshot_chan_cfg_t cfg = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten    = a,
        };
        err = adc_oneshot_config_channel(*h, ch, &cfg);
        if (err != ESP_OK) {
            return fail("gpio.read_adc", "adc_oneshot_config_channel", err);
        }
        if (cached) {
            cache[ch] = (int8_t)a;
        }
    }
    err = adc_oneshot_read(*h, ch, raw);
    return (err == ESP_OK) ? NULL : fail("gpio.read_adc", "adc_oneshot_read", err);
}

///////////////////////////////////////////////////////////////////////////////
// PWM (LEDC)
///////////////////////////////////////////////////////////////////////////////

const char* drv_pwm_setup (int ch, int freq, int res) {
    init();
    if (ch<0 || ch>=LEDC_CHANNEL_MAX) {
        return drv_errf("gpio.setup_pwm : invalid LEDC channel %d", ch);
    }
    if (freq <= 0) {
        return drv_errf("gpio.setup_pwm : frequency must be positive");
    }
    if (res<1 || res>20) {
        return drv_errf("gpio.setup_pwm : resolution must be between 1 and 20");
    }
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = (ledc_timer_t)(ch % 4),
        .duty_resolution = (ledc_timer_bit_t)res,
        .freq_hz         = freq,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return fail("gpio.setup_pwm", "ledc_timer_config", err);
    }
    if (!s_fade_installed) {
        err = ledc_fade_func_install(0);
        if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) {
            return fail("gpio.setup_pwm", "ledc_fade_func_install", err);
        }
        s_fade_installed = true;
    }
    if (s_queue == NULL) {
        return drv_errf("gpio.setup_pwm : failed to initialize the event queue");
    }
    ledc_cbs_t cbs = { .fade_cb = fade_cb };
    err = ledc_cb_register(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, &cbs, NULL);
    if (err != ESP_OK) {
        return fail("gpio.setup_pwm", "ledc_cb_register", err);
    }
    s_pwm[ch].configured = true;
    s_pwm[ch].freq       = freq;
    s_pwm[ch].resolution = res;
    return NULL;
}

const char* drv_pwm_attach (int pin, int ch) {
    init();
    if (!valid_output_pin(pin)) {
        return drv_errf("gpio.attach_pwm : GPIO pin %d cannot be used as output", pin);
    }
    const char* e = bad_channel("gpio.attach_pwm", ch);
    if (e != NULL) {
        return e;
    }
    if (s_pin_pwm[pin] == ch) {
        return NULL;
    }
    if (s_pin_pwm[pin] != -1) {
        detach(pin);
    }
    esp_err_t err;
    if (!s_pwm[ch].initialized) {
        ledc_channel_config_t conf = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)ch,
            .timer_sel  = (ledc_timer_t)(ch % 4),
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = pin,
            .duty       = 0,
            .hpoint     = 0,
        };
        err = ledc_channel_config(&conf);
        if (err != ESP_OK) {
            return fail("gpio.attach_pwm", "ledc_channel_config", err);
        }
        s_pwm[ch].initialized = true;
    } else {
        err = ledc_set_pin(pin, LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
        if (err != ESP_OK) {
            return fail("gpio.attach_pwm", "ledc_set_pin", err);
        }
    }
    s_pin_pwm[pin] = ch;
    return NULL;
}

const char* drv_pwm_detach (int pin) {
    init();
    if (!valid_pin(pin)) {
        return drv_errf("gpio.detach_pwm : invalid GPIO pin %d", pin);
    }
    if (s_pin_pwm[pin] != -1) {
        detach(pin);
    } else {
        (void) gpio_reset_pin((gpio_num_t)pin);
    }
    return NULL;
}

const char* drv_pwm_stop (int ch) {
    if (ch<0 || ch>=LEDC_CHANNEL_MAX) {
        return drv_errf("gpio.stop_pwm : invalid LEDC channel %d", ch);
    }
    esp_err_t err = ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
    return (err == ESP_OK) ? NULL : fail("gpio.stop_pwm", "ledc_stop", err);
}

const char* drv_pwm_write (int ch, int duty) {
    const char* e = bad_channel("gpio.write_pwm", ch);
    if (e != NULL) {
        return e;
    }
    if (duty<0 || duty>(1<<s_pwm[ch].resolution)-1) {
        return drv_errf("gpio.write_pwm : duty out of range for channel resolution");
    }
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, duty);
    if (err != ESP_OK) {
        return fail("gpio.write_pwm", "ledc_set_duty", err);
    }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
    return (err == ESP_OK) ? NULL : fail("gpio.write_pwm", "ledc_update_duty", err);
}

const char* drv_pwm_fade (int ch, int duty, int ms) {
    const char* e = bad_channel("gpio.fade_pwm", ch);
    if (e != NULL) {
        return e;
    }
    if (duty<0 || duty>(1<<s_pwm[ch].resolution)-1) {
        return drv_errf("gpio.fade_pwm : target_duty out of range for channel resolution");
    }
    if (ms < 0) {
        return drv_errf("gpio.fade_pwm : duration must be non-negative");
    }
    esp_err_t err = ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
                                            (ledc_channel_t)ch, duty, ms);
    if (err != ESP_OK) {
        return fail("gpio.fade_pwm", "ledc_set_fade_with_time", err);
    }
    err = ledc_fade_start(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, LEDC_FADE_NO_WAIT);
    return (err == ESP_OK) ? NULL : fail("gpio.fade_pwm", "ledc_fade_start", err);
}
