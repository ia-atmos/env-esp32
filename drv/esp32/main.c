// The entry of the board: initializes the system, then runs the Atmos
// program with the generic driver of the runtime (rt/main.c of
// ia-atmos/lang, compiled with `-Dmain=atmos_main`), which calls the
// `step` of the environments of env/esp32.atm.

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "drv.h"

#define TAG "atmos"

int  atmos_main (int argc, char** argv);
void drv_clock_init (void);

void app_main (void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // NVS: needed by Wi-Fi (ESP-NOW) and by the preferences
    esp_err_t err = nvs_flash_init();
    if (err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    drv_clock_init();

    char* argv[] = { "atmos", NULL };
    int st = atmos_main(1, argv);
    ESP_LOGI(TAG, "program terminated with status %d", st);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
