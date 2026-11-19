// The clock on the ESP32: drv/drv.h over esp_timer and FreeRTOS.
//
// A wait blocks on a binary semaphore, which the drivers give when they
// queue an event (also from an interrupt), with the time to the nearest
// timer of the program as the timeout. While it blocks, the idle task
// runs and feeds the watchdog.

#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "drv.h"

static SemaphoreHandle_t s_wake = NULL;

void drv_clock_init (void) {
    if (s_wake == NULL) {
        s_wake = xSemaphoreCreateBinary();
    }
}

int64_t drv_clock_now (void) {
    return esp_timer_get_time();
}

void IRAM_ATTR drv_clock_wake (void) {
    if (s_wake == NULL) {
        return;
    }
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_wake, &woken);
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    } else {
        xSemaphoreGive(s_wake);
    }
}

int64_t drv_clock_wait (int64_t max_us) {
    int64_t t0 = esp_timer_get_time();
    if (max_us < 0) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
    } else {
        int64_t tick_us = 1000000 / configTICK_RATE_HZ;
        TickType_t ticks = (TickType_t) ((max_us + tick_us - 1) / tick_us);
        if (xSemaphoreTake(s_wake, ticks) != pdTRUE && max_us < tick_us) {
            // less than a tick: the rest, busy
            int64_t left = max_us - (esp_timer_get_time() - t0);
            if (left > 0) {
                esp_rom_delay_us((uint32_t) left);
            }
        }
    }
    return esp_timer_get_time() - t0;
}

