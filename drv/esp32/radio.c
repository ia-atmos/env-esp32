// The Wi-Fi radio, shared by Wi-Fi and ESP-NOW: see radio.h.

#include "esp_wifi.h"

#include "radio.h"

static unsigned s_users = 0;

void radio_use (radio_user_t user) {
    s_users |= (unsigned) user;
}

bool radio_release (radio_user_t user) {
    s_users &= ~(unsigned) user;
    return s_users == 0;
}

void radio_off (void) {
    (void) esp_wifi_stop();
    (void) esp_wifi_set_mode(WIFI_MODE_NULL);
}
