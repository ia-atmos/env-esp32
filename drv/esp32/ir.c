// The IR receiver on the ESP32 (RMT): drv/drv.h over ESP-IDF.
// From main/ir/ir.c (the Lua version), with the Lua API taken out.

#include "decoder.h"
#include "drv.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define IR_RX_RESOLUTION_HZ       1000000U
#define IR_RX_SYMBOLS             96
#define IR_CAPTURE_QUEUE_LEN      1
#define IR_EVENT_QUEUE_LEN        16
#define IR_WORKER_STACK_WORDS     1536
#define IR_WORKER_PRIORITY        10
#define IR_DEFAULT_GLITCH_NS      1250U
#define IR_DEFAULT_IDLE_NS        15000000U

typedef struct {
    size_t symbol_count;
    int64_t time_us;
} ir_capture_t;

typedef struct {
    ir_frame_t frame;
    int64_t time_us;
} ir_queued_event_t;

typedef struct {
    uint32_t captured;
    uint32_t decoded;
    uint32_t invalid;
    uint32_t dropped;
} ir_stats_t;

static const char *TAG = "ir";

static rmt_channel_handle_t s_rx_channel;
static rmt_symbol_word_t s_rx_symbols[IR_RX_SYMBOLS];
static rmt_receive_config_t s_receive_config;
static QueueHandle_t s_capture_queue;
static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_worker_stopped;
static TaskHandle_t s_worker_task;
static volatile bool s_running;
static ir_stats_t s_stats;
static ir_frame_t s_last_frame;
static int64_t s_last_frame_time_us;
static bool s_have_last_frame;

static bool IRAM_ATTR ir_rx_done_callback(rmt_channel_handle_t channel,
                                          const rmt_rx_done_event_data_t *edata,
                                          void *user_data)
{
    (void)channel;
    (void)user_data;

    if (!s_running || s_capture_queue == NULL) {
        return false;
    }

    ir_capture_t capture = {
        .symbol_count = edata->num_symbols,
        .time_us = esp_timer_get_time(),
    };
    BaseType_t task_woken = pdFALSE;
    if (xQueueSendFromISR(s_capture_queue, &capture, &task_woken) != pdTRUE) {
        ++s_stats.dropped;
    }
    return task_woken == pdTRUE;
}

static size_t flatten_symbols(const rmt_symbol_word_t *symbols, size_t symbol_count,
                              ir_pulse_t *pulses, size_t pulse_capacity)
{
    size_t pulse_count = 0;
    for (size_t i = 0; i < symbol_count && pulse_count < pulse_capacity; ++i) {
        if (symbols[i].duration0 > 0) {
            pulses[pulse_count++] = (ir_pulse_t) {
                .duration_us = symbols[i].duration0,
                .level = symbols[i].level0,
            };
        }
        if (symbols[i].duration1 > 0 && pulse_count < pulse_capacity) {
            pulses[pulse_count++] = (ir_pulse_t) {
                .duration_us = symbols[i].duration1,
                .level = symbols[i].level1,
            };
        }
    }
    return pulse_count;
}

static void ir_worker(void *arg)
{
    (void)arg;
    ir_capture_t capture;
    ir_pulse_t pulses[IR_RX_SYMBOLS * 2];

    while (xQueueReceive(s_capture_queue, &capture, portMAX_DELAY) == pdTRUE) {
        if (!s_running) {
            break;
        }

        ++s_stats.captured;
        size_t pulse_count = flatten_symbols(s_rx_symbols, capture.symbol_count,
                                             pulses, IR_RX_SYMBOLS * 2);
        ir_frame_t frame;
        if (ir_decode(pulses, pulse_count, &frame)) {
            if (frame.repeat) {
                if (s_have_last_frame &&
                    s_last_frame.protocol == frame.protocol) {
                    ir_frame_t repeated = s_last_frame;
                    repeated.repeat = true;
                    frame = repeated;
                } else {
                    ++s_stats.invalid;
                    goto rearm;
                }
            } else {
                /*
                 * Sony repeats complete frames instead of sending a special
                 * repeat code. Treat an identical frame arriving within
                 * 120 ms as a repeat while preserving its decoded contents.
                 */
                if (s_have_last_frame &&
                    s_last_frame.protocol == frame.protocol &&
                    s_last_frame.raw == frame.raw &&
                    capture.time_us - s_last_frame_time_us <= 120000) {
                    frame.repeat = true;
                }
                s_last_frame = frame;
                s_last_frame.repeat = false;
                s_last_frame_time_us = capture.time_us;
                s_have_last_frame = true;
            }

            ir_queued_event_t event = {
                .frame = frame,
                .time_us = capture.time_us,
            };
            if (xQueueSend(s_event_queue, &event, 0) == pdTRUE) {
                ++s_stats.decoded;
                drv_clock_wake();
            } else {
                ++s_stats.dropped;
            }
        } else {
            ++s_stats.invalid;
        }

rearm:
        if (s_running) {
            esp_err_t err = rmt_receive(s_rx_channel, s_rx_symbols,
                                        sizeof(s_rx_symbols), &s_receive_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to re-arm receiver: %s", esp_err_to_name(err));
                s_running = false;
            }
        }
    }

    s_worker_task = NULL;
    xSemaphoreGive(s_worker_stopped);
    vTaskDelete(NULL);
}

static void destroy_driver(void)
{
    if (s_rx_channel != NULL) {
        (void)rmt_disable(s_rx_channel);
        (void)rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    if (s_capture_queue != NULL) {
        vQueueDelete(s_capture_queue);
        s_capture_queue = NULL;
    }
    if (s_event_queue != NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_worker_stopped != NULL) {
        vSemaphoreDelete(s_worker_stopped);
        s_worker_stopped = NULL;
    }
}

const char* drv_ir_start (int pin, int64_t glitch, int64_t idle)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return drv_errf("ir.start : invalid GPIO pin %d", pin);
    }
    if (s_running || s_rx_channel != NULL) {
        return drv_errf("ir.start : IR receiver is already running");
    }

    uint32_t glitch_ns = (glitch < 0) ? IR_DEFAULT_GLITCH_NS : (uint32_t)glitch;
    uint32_t idle_ns   = (idle   < 0) ? IR_DEFAULT_IDLE_NS   : (uint32_t)idle;
    if (glitch_ns > 3187) {
        return drv_errf("ir.start : glitch_ns must be <= 3187 ns "
                        "(ESP32 hardware filter limit)");
    }
    if (glitch_ns >= idle_ns) {
        return drv_errf("ir.start : expected glitch_ns < idle_ns");
    }

    s_capture_queue = xQueueCreate(IR_CAPTURE_QUEUE_LEN, sizeof(ir_capture_t));
    s_event_queue = xQueueCreate(IR_EVENT_QUEUE_LEN, sizeof(ir_queued_event_t));
    s_worker_stopped = xSemaphoreCreateBinary();
    if (s_capture_queue == NULL || s_event_queue == NULL || s_worker_stopped == NULL) {
        destroy_driver();
        return drv_errf("ir.start : failed to allocate IR driver queues");
    }

    rmt_rx_channel_config_t channel_config = {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RX_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .intr_priority = 0,
        .flags = {
            .invert_in = false,
            .with_dma = false,
            .allow_pd = false,
        },
    };
    esp_err_t err = rmt_new_rx_channel(&channel_config, &s_rx_channel);
    if (err != ESP_OK) {
        destroy_driver();
        return drv_errf("ir.start : rmt_new_rx_channel failed: %s", esp_err_to_name(err));
    }

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = ir_rx_done_callback,
    };
    err = rmt_rx_register_event_callbacks(s_rx_channel, &callbacks, NULL);
    if (err == ESP_OK) {
        err = rmt_enable(s_rx_channel);
    }
    if (err != ESP_OK) {
        destroy_driver();
        return drv_errf("ir.start : failed to enable IR receiver: %s", esp_err_to_name(err));
    }

    s_receive_config = (rmt_receive_config_t) {
        .signal_range_min_ns = glitch_ns,
        .signal_range_max_ns = idle_ns,
        .flags.en_partial_rx = false,
    };
    memset(&s_stats, 0, sizeof(s_stats));
    s_have_last_frame = false;
    s_running = true;

    if (xTaskCreate(ir_worker, "ir_rx", IR_WORKER_STACK_WORDS, NULL,
                    IR_WORKER_PRIORITY, &s_worker_task) != pdPASS) {
        s_running = false;
        destroy_driver();
        return drv_errf("ir.start : failed to create IR decoder task");
    }

    err = rmt_receive(s_rx_channel, s_rx_symbols, sizeof(s_rx_symbols),
                      &s_receive_config);
    if (err != ESP_OK) {
        s_running = false;
        ir_capture_t wake = {0};
        (void)xQueueSend(s_capture_queue, &wake, 0);
        (void)xSemaphoreTake(s_worker_stopped, pdMS_TO_TICKS(500));
        destroy_driver();
        return drv_errf("ir.start : failed to start IR reception: %s", esp_err_to_name(err));
    }

    return NULL;
}

const char* drv_ir_stop (void)
{
    if (s_rx_channel == NULL) {
        return NULL;
    }

    s_running = false;
    (void)rmt_disable(s_rx_channel);
    ir_capture_t wake = {0};
    (void)xQueueOverwrite(s_capture_queue, &wake);
    if (s_worker_task != NULL &&
        xSemaphoreTake(s_worker_stopped, pdMS_TO_TICKS(500)) != pdTRUE) {
        return drv_errf("ir.stop : timed out while stopping IR receiver");
    }
    destroy_driver();
    return NULL;
}

bool drv_ir_next (drv_evt_t* evt)
{
    ir_queued_event_t event;
    if (s_event_queue == NULL || xQueueReceive(s_event_queue, &event, 0) != pdTRUE) {
        return false;
    }
    evt->kind = DRV_EVT_IR;
    evt->time_us = event.time_us;
    evt->u.ir.protocol = ir_protocol_name(event.frame.protocol);
    evt->u.ir.address  = event.frame.address;
    evt->u.ir.command  = event.frame.command;
    evt->u.ir.raw      = event.frame.raw;
    evt->u.ir.bits     = event.frame.bits;
    evt->u.ir.repeat   = event.frame.repeat;
    return true;
}

void drv_ir_stats (drv_ir_stats_t* stats)
{
    stats->captured = s_stats.captured;
    stats->decoded  = s_stats.decoded;
    stats->invalid  = s_stats.invalid;
    stats->dropped  = s_stats.dropped;
}
