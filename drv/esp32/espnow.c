// ESP-NOW on the ESP32: drv/drv.h over ESP-IDF (Wi-Fi in station mode).
// From main/espnow.c (the Lua version), with the Lua API taken out.
//
// The callbacks of ESP-NOW run in the task of Wi-Fi, not in an
// interrupt: they queue with xQueueSend (the Lua version used the
// FromISR variants).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "drv.h"

#define QUEUE_LEN 16

_Static_assert(DRV_ESPNOW_MAX_DATA == ESP_NOW_MAX_DATA_LEN, "ESP-NOW payload");

static QueueHandle_t s_queue = NULL;
static bool s_inited = false;

static bool parse_mac (const char* s, uint8_t mac[6]) {
    unsigned v[6];
    char end;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&end) != 6) {
        return false;
    }
    for (int i=0; i<6; i++) {
        if (v[i] > 0xFF) {
            return false;
        }
        mac[i] = (uint8_t) v[i];
    }
    return true;
}

static void fmt_mac (const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void recv_cb (const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (s_queue == NULL) {
        return;
    }
    drv_espnow_evt_t e = { .kind=DRV_ESPNOW_RECV, .time_us=esp_timer_get_time(), .ok=true };
    fmt_mac(info->src_addr, e.mac);
    if (len > DRV_ESPNOW_MAX_DATA) {
        len = DRV_ESPNOW_MAX_DATA;
    }
    if (len>0 && data!=NULL) {
        memcpy(e.data, data, len);
    }
    e.len = (len > 0) ? len : 0;
    (void) xQueueSend(s_queue, &e, 0);
    drv_clock_wake();
}

static void send_cb (const esp_now_send_info_t* info, esp_now_send_status_t status) {
    if (s_queue == NULL) {
        return;
    }
    drv_espnow_evt_t e = { .kind=DRV_ESPNOW_SEND, .time_us=esp_timer_get_time() };
    e.ok = (status == ESP_NOW_SEND_SUCCESS);
    fmt_mac(info->des_addr, e.mac);
    (void) xQueueSend(s_queue, &e, 0);
    drv_clock_wake();
}

static esp_err_t init_wifi (void) {
    esp_err_t err = esp_netif_init();
    if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL &&
        esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) {
        return err;
    }
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA))      != ESP_OK) return err;
    if ((err = esp_wifi_start())                      != ESP_OK) return err;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE))         != ESP_OK) return err;
    return esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

static const char* fail (const char* fn, const char* what, esp_err_t err) {
    return drv_errf("%s : %s: %s", fn, what, esp_err_to_name(err));
}

const char* drv_espnow_init (void) {
    if (s_inited) {
        return NULL;
    }
    esp_err_t err = init_wifi();
    if (err != ESP_OK) {
        return fail("espnow.init", "failed to initialize wifi", err);
    }
    err = esp_now_init();
    if (err != ESP_OK) {
        return fail("espnow.init", "failed to initialize esp-now", err);
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(QUEUE_LEN, sizeof(drv_espnow_evt_t));
        if (s_queue == NULL) {
            return drv_errf("espnow.init : failed to create the event queue");
        }
    }
    if ((err = esp_now_register_recv_cb(recv_cb)) != ESP_OK) {
        return fail("espnow.init", "failed to register recv callback", err);
    }
    if ((err = esp_now_register_send_cb(send_cb)) != ESP_OK) {
        return fail("espnow.init", "failed to register send callback", err);
    }
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    if (!esp_now_is_peer_exist(bcast)) {
        esp_now_peer_info_t peer = { .channel=0, .ifidx=WIFI_IF_STA, .encrypt=false };
        memcpy(peer.peer_addr, bcast, 6);
        (void) esp_now_add_peer(&peer);
    }
    s_inited = true;
    return NULL;
}

const char* drv_espnow_add_peer (const char* s) {
    if (!s_inited) {
        return drv_errf("espnow.add_peer : esp-now is not initialized");
    }
    uint8_t mac[6];
    if (!parse_mac(s, mac)) {
        return drv_errf("espnow.add_peer : invalid MAC address format '%s'", s);
    }
    if (esp_now_is_peer_exist(mac)) {
        return NULL;
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    esp_err_t err = esp_now_add_peer(&peer);
    return (err == ESP_OK) ? NULL
         : fail("espnow.add_peer", "failed to add esp-now peer", err);
}

const char* drv_espnow_send (const char* s, const uint8_t* data, int len) {
    if (!s_inited) {
        return drv_errf("espnow.send : esp-now is not initialized");
    }
    uint8_t mac[6];
    if (!parse_mac(s, mac)) {
        return drv_errf("espnow.send : invalid MAC address format '%s'", s);
    }
    if (len > DRV_ESPNOW_MAX_DATA) {
        return drv_errf("espnow.send : data length %d exceeds max ESP-NOW "
                        "payload of %d bytes", len, DRV_ESPNOW_MAX_DATA);
    }
    esp_err_t err = esp_now_send(mac, data, len);
    return (err == ESP_OK) ? NULL : fail("espnow.send", "esp_now_send failed", err);
}

const char* drv_espnow_get_mac (char out[18]) {
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            return fail("espnow.get_mac", "failed to get local MAC address", err);
        }
    }
    fmt_mac(mac, out);
    return NULL;
}

const char* drv_espnow_get_channel (int* channel) {
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_channel(&primary, &second);
    if (err != ESP_OK) {
        return fail("espnow.get_channel", "failed to get wifi channel", err);
    }
    *channel = primary;
    return NULL;
}

const char* drv_espnow_set_channel (int channel) {
    if (channel<1 || channel>14) {
        return drv_errf("espnow.set_channel : invalid Wi-Fi channel (1-14)");
    }
    esp_err_t err = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    return (err == ESP_OK) ? NULL
         : fail("espnow.set_channel", "failed to set wifi channel", err);
}

bool drv_espnow_next (drv_espnow_evt_t* evt) {
    return s_queue!=NULL && xQueueReceive(s_queue, evt, 0)==pdTRUE;
}
