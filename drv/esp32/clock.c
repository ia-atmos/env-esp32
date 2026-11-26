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
#include "freertos/task.h"

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

// The end of the previous wait: the time that the program spends
// between two waits passes too, and is discounted from the next wait.
static int64_t s_last = -1;

void drv_clock_sync (void) {
    s_last = esp_timer_get_time();
}

int64_t drv_clock_wait (int64_t max_us) {
    int64_t t0 = esp_timer_get_time();
    if (s_last < 0) {
        s_last = t0;
    }
    if (max_us >= 0) {
        max_us -= t0 - s_last;
        if (max_us < 0) {
            max_us = 0;
        }
    }
#ifdef ESP32_FPS
    // Never blocks: the steps run as fast as possible (`:fps`). Once a
    // second, yields a tick, so that the idle task feeds the watchdog.
    (void) max_us;
    static int64_t s_yield = 0;
    if (t0 - s_yield >= 1000000) {
        vTaskDelay(1);
        s_yield = esp_timer_get_time();
    }
#else
    if (max_us < 0) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
    } else if (max_us > 0) {
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
#endif
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_last;
    s_last = now;
    return dt;
}
