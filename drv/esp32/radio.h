// The Wi-Fi radio, shared by drv/esp32/wifi.c and drv/esp32/espnow.c.
//
// Each driver marks the radio as used when it starts it, and releases it
// when it stops; the radio turns off (esp_wifi_stop) only when the last
// user releases it, so that wifi.stop does not end ESP-NOW, nor
// espnow.stop end Wi-Fi. Called only from the main task (the Atmos
// program), so with no lock.

#ifndef DRV_ESP32_RADIO_H
#define DRV_ESP32_RADIO_H

#include <stdbool.h>

typedef enum {
    RADIO_WIFI   = 1 << 0,
    RADIO_ESPNOW = 1 << 1,
} radio_user_t;

void radio_use     (radio_user_t user);

// Gives true if no other user is left: the caller then turns the radio off.
bool radio_release (radio_user_t user);

// Turns the radio off and leaves no mode behind, so that the next start
// (of Wi-Fi or ESP-NOW) does not bring back a SoftAP of before.
void radio_off     (void);

#endif
