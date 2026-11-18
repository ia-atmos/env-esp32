// The free memory on the ESP32: drv/drv.h over heap_caps.
// From main/main.c (the Lua version, `_esp.meminfo`).

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drv.h"

void drv_meminfo (drv_meminfo_t* m) {
    m->heap_total            = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    m->heap_free             = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    m->heap_min_free         = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    m->heap_largest_free     = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    m->internal_total        = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    m->internal_free         = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m->internal_min_free     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    m->internal_largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    m->stack_free            = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
}
