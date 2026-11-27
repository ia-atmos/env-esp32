// Wi-Fi on the ESP32 (STA, SoftAP, and AP scan): drv/drv.h over ESP-IDF.
//
// Event handlers run in the ESP default event loop task (not an ISR):
// they update cached state, push a drv_wifi_evt_t into the FreeRTOS queue,
// and wake the Atmos clock semaphore with drv_clock_wake().
//
// Coexistence with ESP-NOW (drv/esp32/espnow.c):
//   - Both share the default netif/event loop/esp_wifi driver safely.
//   - Starting SoftAP while STA (or ESP-NOW) is active uses WIFI_MODE_APSTA
//     so STA/ESP-NOW stays alive; stopping SoftAP drops back to WIFI_MODE_STA.
//   - Stopping Wi-Fi (drv_wifi_stop) ends STA, SoftAP and scan; the radio
//     turns off only if ESP-NOW does not use it (radio.h).
//   - A stop tells the program of everything it ends, exactly once:
//     drv_wifi_stop queues the DISCONNECTED of the STA (the one that
//     ESP-IDF posts afterwards, in ST_STOPPED, is dropped) and a SCAN_DONE
//     "stopped" for a scan in progress (a late SCAN_DONE, with no scan in
//     progress, is dropped); drv_wifi_stop and drv_wifi_stop_ap queue an
//     AP_DISCONNECTED for each SoftAP client that the program saw connect
//     (`s_clients`: a client leaves it by its AP_DISCONNECTED, from
//     ESP-IDF or from the stop, whichever comes first; with no SoftAP,
//     the AP events of ESP-IDF are dropped). The reason is 8 (ASSOC_LEAVE).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "drv.h"
#include "radio.h"

#define WIFI_QUEUE_LEN       16
#define WIFI_SCAN_DEF_MAX    20

typedef enum {
    ST_STOPPED = 0,
    ST_DISCONNECTED,
    ST_CONNECTING,
    ST_CONNECTED,
    ST_GOT_IP,
} wifi_st_t;

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_lock = NULL;
static esp_event_handler_instance_t s_wifi_hdl = NULL;
static esp_event_handler_instance_t s_ip_hdl = NULL;
static bool s_inited = false;
static bool s_started = false;
static bool s_sta_active = false;
static bool s_ap_active = false;
static bool s_scanning = false;
static int  s_scan_max = WIFI_SCAN_DEF_MAX;

static wifi_st_t s_state = ST_STOPPED;
static char s_ssid[33];
static char s_bssid[18];
static char s_ip[16];
static char s_netmask[16];
static char s_gateway[16];
static int  s_channel = 0;
static char s_ap_ssid[33];

static drv_wifi_ap_t s_scan_aps[DRV_WIFI_SCAN_MAX];
static int           s_scan_n = 0;

// The SoftAP clients that the program saw connect (under s_lock).
#define AP_CLIENTS_MAX 10           // max_conn of drv_wifi_start_ap
typedef struct {
    char mac[18];
    int  aid;
} ap_client_t;
static ap_client_t s_clients[AP_CLIENTS_MAX];
static int         s_clients_n = 0;

static void lock_take (void) {
    if (s_lock != NULL) {
        (void) xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void lock_give (void) {
    if (s_lock != NULL) {
        (void) xSemaphoreGive(s_lock);
    }
}

static const char* state_name (wifi_st_t st) {
    switch (st) {
        case ST_STOPPED:      return "stopped";
        case ST_DISCONNECTED: return "disconnected";
        case ST_CONNECTING:   return "connecting";
        case ST_CONNECTED:    return "connected";
        case ST_GOT_IP:       return "got_ip";
    }
    return "stopped";
}

static const char* auth_name (wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa_wpa2";
        case WIFI_AUTH_ENTERPRISE:      return "enterprise";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2_wpa3";
        case WIFI_AUTH_WAPI_PSK:        return "wapi";
        case WIFI_AUTH_OWE:             return "owe";
        default:                        return "unknown";
    }
}

static bool parse_mac (const char* s, uint8_t mac[6]) {
    unsigned v[6];
    char end;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &end) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
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

static void copy_ssid (const uint8_t* src, size_t len, char dst[33]) {
    if (len > 32) {
        len = 32;
    }
    if (len > 0 && src != NULL) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static const char* fail (const char* fn, const char* what, esp_err_t err) {
    return drv_errf("%s : %s: %s", fn, what, esp_err_to_name(err));
}

static void queue_evt (const drv_wifi_evt_t* e) {
    if (s_queue != NULL) {
        (void) xQueueSend(s_queue, e, 0);
        drv_clock_wake();
    }
}

// Under s_lock.
static void clients_add (const char* mac, int aid) {
    if (s_clients_n < AP_CLIENTS_MAX) {
        strcpy(s_clients[s_clients_n].mac, mac);
        s_clients[s_clients_n].aid = aid;
        s_clients_n++;
    }
}

// Under s_lock.
static void clients_remove (const char* mac) {
    for (int i = 0; i < s_clients_n; i++) {
        if (!strcmp(s_clients[i].mac, mac)) {
            s_clients[i] = s_clients[--s_clients_n];
            return;
        }
    }
}

// Under s_lock: the clients that a stop disconnects, which leave the table.
static int clients_take (ap_client_t out[AP_CLIENTS_MAX]) {
    int n = s_clients_n;
    memcpy(out, s_clients, (size_t)n * sizeof(ap_client_t));
    s_clients_n = 0;
    return n;
}

static void queue_clients_gone (const ap_client_t* c, int n) {
    for (int i = 0; i < n; i++) {
        drv_wifi_evt_t e = { .kind = DRV_WIFI_AP_DISCONNECTED,
                             .time_us = esp_timer_get_time(),
                             .aid = c[i].aid, .reason = 8 };     // ASSOC_LEAVE
        strcpy(e.bssid, c[i].mac);
        queue_evt(&e);
    }
}

static void on_wifi_event (void* arg, esp_event_base_t base,
                           int32_t id, void* data) {
    (void) arg;
    (void) base;
    int64_t now = esp_timer_get_time();

    switch (id) {
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t* ev = (const wifi_event_sta_connected_t*) data;
            drv_wifi_evt_t e = { .kind = DRV_WIFI_CONNECTED, .time_us = now };
            copy_ssid(ev->ssid, ev->ssid_len, e.ssid);
            fmt_mac(ev->bssid, e.bssid);
            e.channel = ev->channel;

            lock_take();
            s_state = ST_CONNECTED;
            strcpy(s_ssid, e.ssid);
            strcpy(s_bssid, e.bssid);
            s_channel = e.channel;
            lock_give();

            queue_evt(&e);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t* ev = (const wifi_event_sta_disconnected_t*) data;
            drv_wifi_evt_t e = { .kind = DRV_WIFI_DISCONNECTED, .time_us = now };
            copy_ssid(ev->ssid, ev->ssid_len, e.ssid);
            fmt_mac(ev->bssid, e.bssid);
            e.reason = ev->reason;

            lock_take();
            if (s_state == ST_STOPPED) {
                // the echo of drv_wifi_stop, which queued its own
                lock_give();
                break;
            }
            if (s_started) {
                s_state = ST_DISCONNECTED;
            }
            s_ssid[0] = '\0';
            s_bssid[0] = '\0';
            s_ip[0] = '\0';
            s_netmask[0] = '\0';
            s_gateway[0] = '\0';
            s_channel = 0;
            lock_give();

            queue_evt(&e);
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            const wifi_event_sta_scan_done_t* ev = (const wifi_event_sta_scan_done_t*) data;
            bool failed = (ev != NULL && ev->status != 0);
            uint16_t ap_num = 0;
            if (!failed) {
                (void) esp_wifi_scan_get_ap_num(&ap_num);
            }
            int cap = s_scan_max;
            if (cap <= 0 || cap > DRV_WIFI_SCAN_MAX) {
                cap = DRV_WIFI_SCAN_MAX;
            }
            uint16_t count = (ap_num > (uint16_t)cap) ? (uint16_t)cap : ap_num;
            wifi_ap_record_t* recs = NULL;
            if (count > 0) {
                recs = (wifi_ap_record_t*) calloc(count, sizeof(wifi_ap_record_t));
                if (recs != NULL) {
                    if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK) {
                        count = 0;
                    }
                } else {
                    (void) esp_wifi_clear_ap_list();
                    count = 0;
                }
            } else {
                (void) esp_wifi_clear_ap_list();
            }

            lock_take();
            if (!s_scanning) {
                // no scan in progress: drv_wifi_stop already queued its end
                lock_give();
                free(recs);
                break;
            }
            s_scanning = false;
            s_scan_n = (int) count;
            for (int i = 0; i < s_scan_n; i++) {
                copy_ssid(recs[i].ssid, strnlen((const char*)recs[i].ssid, 32),
                          s_scan_aps[i].ssid);
                fmt_mac(recs[i].bssid, s_scan_aps[i].bssid);
                s_scan_aps[i].rssi = recs[i].rssi;
                s_scan_aps[i].channel = recs[i].primary;
                s_scan_aps[i].auth = auth_name(recs[i].authmode);
            }
            lock_give();
            free(recs);

            drv_wifi_evt_t e = {
                .kind = DRV_WIFI_SCAN_DONE,
                .time_us = now,
                .scan_count = (int) count,
                .scan_status = failed ? "fail" : "success",
            };
            queue_evt(&e);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t* ev = (const wifi_event_ap_staconnected_t*) data;
            drv_wifi_evt_t e = { .kind = DRV_WIFI_AP_CONNECTED, .time_us = now };
            fmt_mac(ev->mac, e.bssid);
            e.aid = ev->aid;
            lock_take();
            bool on = s_ap_active;
            if (on) {
                clients_add(e.bssid, e.aid);
            }
            lock_give();
            if (on) {
                queue_evt(&e);
            }
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t* ev = (const wifi_event_ap_stadisconnected_t*) data;
            drv_wifi_evt_t e = { .kind = DRV_WIFI_AP_DISCONNECTED, .time_us = now };
            fmt_mac(ev->mac, e.bssid);
            e.aid = ev->aid;
            e.reason = ev->reason;
            lock_take();
            bool on = s_ap_active;      // with no SoftAP: the stop queued it
            if (on) {
                clients_remove(e.bssid);
            }
            lock_give();
            if (on) {
                queue_evt(&e);
            }
            break;
        }
        default:
            break;
    }
}

static void on_ip_event (void* arg, esp_event_base_t base,
                         int32_t id, void* data) {
    (void) arg;
    (void) base;
    int64_t now = esp_timer_get_time();

    switch (id) {
        case IP_EVENT_STA_GOT_IP: {
            const ip_event_got_ip_t* ev = (const ip_event_got_ip_t*) data;
            drv_wifi_evt_t e = { .kind = DRV_WIFI_GOT_IP, .time_us = now };
            esp_ip4addr_ntoa(&ev->ip_info.ip, e.ip, sizeof(e.ip));
            esp_ip4addr_ntoa(&ev->ip_info.netmask, e.netmask, sizeof(e.netmask));
            esp_ip4addr_ntoa(&ev->ip_info.gw, e.gateway, sizeof(e.gateway));
            e.changed = ev->ip_changed;

            lock_take();
            s_state = ST_GOT_IP;
            strcpy(s_ip, e.ip);
            strcpy(s_netmask, e.netmask);
            strcpy(s_gateway, e.gateway);
            lock_give();

            queue_evt(&e);
            break;
        }
        case IP_EVENT_STA_LOST_IP: {
            drv_wifi_evt_t e = { .kind = DRV_WIFI_LOST_IP, .time_us = now };
            lock_take();
            if (s_state == ST_GOT_IP) {
                s_state = ST_CONNECTED;
            }
            s_ip[0] = '\0';
            s_netmask[0] = '\0';
            s_gateway[0] = '\0';
            lock_give();

            queue_evt(&e);
            break;
        }
        case IP_EVENT_ASSIGNED_IP_TO_CLIENT: {
            const ip_event_assigned_ip_to_client_t* ev =
                (const ip_event_assigned_ip_to_client_t*) data;
            drv_wifi_evt_t e = { .kind = DRV_WIFI_AP_STA_IP, .time_us = now };
            esp_ip4addr_ntoa(&ev->ip, e.ip, sizeof(e.ip));
            fmt_mac(ev->mac, e.bssid);
            queue_evt(&e);
            break;
        }
        default:
            break;
    }
}

static bool peek (int64_t* time_us) {
    drv_wifi_evt_t e;
    if (s_queue == NULL || xQueuePeek(s_queue, &e, 0) != pdTRUE) {
        return false;
    }
    *time_us = e.time_us;
    return true;
}

static esp_err_t ensure_init (void) {
    if (s_inited) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(WIFI_QUEUE_LEN, sizeof(drv_wifi_evt_t));
        if (s_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    bool fresh = (err == ESP_OK);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (fresh) {
        if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) {
            return err;
        }
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, NULL, &s_wifi_hdl);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              on_ip_event, NULL, &s_ip_hdl);
    if (err != ESP_OK) {
        return err;
    }
    drv_source_add(DRV_SRC_WIFI, peek);
    s_inited = true;
    return ESP_OK;
}

static esp_netif_t* ensure_sta_netif (void) {
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) {
        n = esp_netif_create_default_wifi_sta();
    }
    return n;
}

static esp_netif_t* ensure_ap_netif (void) {
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (n == NULL) {
        n = esp_netif_create_default_wifi_ap();
    }
    return n;
}

static bool sta_in_use (void) {
    if (s_sta_active) {
        return true;
    }
    // Preserve STA if another module (e.g., ESP-NOW) already activated it.
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) == ESP_OK &&
        (cur == WIFI_MODE_STA || cur == WIFI_MODE_APSTA)) {
        return true;
    }
    return false;
}

const char* drv_wifi_connect (const char* ssid, const char* password,
                              const drv_wifi_connect_opts_t* opts) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return drv_errf("wifi.connect : ssid must be 1 to 32 characters");
    }
    if (password != NULL && strlen(password) > 64) {
        return drv_errf("wifi.connect : password must be at most 64 characters");
    }

    uint8_t bssid[6];
    bool has_bssid = false;
    int channel = 0;
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    bool set_ps = false;

    if (opts != NULL) {
        if (opts->bssid != NULL) {
            if (!parse_mac(opts->bssid, bssid)) {
                return drv_errf("wifi.connect : invalid BSSID format '%s'", opts->bssid);
            }
            has_bssid = true;
        }
        if (opts->channel < 0 || opts->channel > 14) {
            return drv_errf("wifi.connect : invalid Wi-Fi channel (1-14)");
        }
        channel = opts->channel;
        if (opts->power_save != NULL) {
            if (!strcmp(opts->power_save, "none")) {
                ps = WIFI_PS_NONE;
            } else if (!strcmp(opts->power_save, "min")) {
                ps = WIFI_PS_MIN_MODEM;
            } else if (!strcmp(opts->power_save, "max")) {
                ps = WIFI_PS_MAX_MODEM;
            } else {
                return drv_errf("wifi.connect : invalid power_save '%s' "
                                "(expected none, min, max)", opts->power_save);
            }
            set_ps = true;
        }
    }

    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return fail("wifi.connect", "failed to initialize wifi", err);
    }
    esp_netif_t* sta = ensure_sta_netif();
    if (sta == NULL) {
        return drv_errf("wifi.connect : failed to create STA netif");
    }
    if (opts != NULL && opts->hostname != NULL) {
        err = esp_netif_set_hostname(sta, opts->hostname);
        if (err != ESP_OK) {
            return fail("wifi.connect", "failed to set hostname", err);
        }
    }

    wifi_mode_t mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if ((err = esp_wifi_set_mode(mode)) != ESP_OK) {
        return fail("wifi.connect", "failed to set wifi mode", err);
    }

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    memcpy(wcfg.sta.ssid, ssid, strlen(ssid));
    if (password != NULL && password[0] != '\0') {
        memcpy(wcfg.sta.password, password, strlen(password));
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    if (has_bssid) {
        wcfg.sta.bssid_set = true;
        memcpy(wcfg.sta.bssid, bssid, 6);
    }
    if (channel > 0) {
        wcfg.sta.channel = (uint8_t) channel;
    }

    if (s_started && (s_state == ST_CONNECTING || s_state == ST_CONNECTED || s_state == ST_GOT_IP)) {
        (void) esp_wifi_disconnect();
    }
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wcfg)) != ESP_OK) {
        return fail("wifi.connect", "failed to configure STA", err);
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        return fail("wifi.connect", "failed to start wifi", err);
    }
    radio_use(RADIO_WIFI);
    if (set_ps) {
        if ((err = esp_wifi_set_ps(ps)) != ESP_OK) {
            return fail("wifi.connect", "failed to set power save", err);
        }
    }

    lock_take();
    s_started = true;
    s_sta_active = true;
    s_state = ST_CONNECTING;
    lock_give();

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        lock_take();
        s_state = ST_DISCONNECTED;
        lock_give();
        return fail("wifi.connect", "esp_wifi_connect failed", err);
    }
    return NULL;
}

const char* drv_wifi_disconnect (void) {
    if (!s_started || !s_sta_active) {
        return NULL;
    }
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return fail("wifi.disconnect", "esp_wifi_disconnect failed", err);
    }
    return NULL;
}

const char* drv_wifi_stop (void) {
    if (!s_inited) {
        return NULL;
    }
    drv_wifi_evt_t e = { .kind = DRV_WIFI_DISCONNECTED, .reason = 8 };  // ASSOC_LEAVE
    lock_take();
    bool sta = (s_state == ST_CONNECTING || s_state == ST_CONNECTED || s_state == ST_GOT_IP);
    bool ap = s_ap_active;
    bool scanning = s_scanning;
    ap_client_t gone[AP_CLIENTS_MAX];
    int ngone = clients_take(gone);
    if (scanning) {
        s_scan_n = 0;
    }
    strcpy(e.ssid, s_ssid);
    strcpy(e.bssid, s_bssid);
    s_started = false;
    s_sta_active = false;
    s_ap_active = false;
    s_scanning = false;
    s_state = ST_STOPPED;       // from now, the handler drops DISCONNECTED,
                                // SCAN_DONE and the AP events
    s_ssid[0] = '\0';
    s_bssid[0] = '\0';
    s_ip[0] = '\0';
    s_netmask[0] = '\0';
    s_gateway[0] = '\0';
    s_ap_ssid[0] = '\0';
    s_channel = 0;
    lock_give();

    esp_err_t err = ESP_OK;
    if (radio_release(RADIO_WIFI)) {
        radio_off();
    } else {
        // ESP-NOW keeps the radio: undo only what Wi-Fi started
        if (scanning) {
            (void) esp_wifi_scan_stop();
        }
        if (sta) {
            err = esp_wifi_disconnect();
        }
        if (ap && err == ESP_OK) {
            err = esp_wifi_set_mode(WIFI_MODE_STA);
        }
        (void) esp_wifi_set_ps(WIFI_PS_NONE);   // as drv_espnow_init
    }
    if (scanning) {
        drv_wifi_evt_t d = { .kind = DRV_WIFI_SCAN_DONE,
                             .time_us = esp_timer_get_time(), .scan_count = 0,
                             .scan_status = "stopped" };
        queue_evt(&d);
    }
    queue_clients_gone(gone, ngone);
    if (sta) {
        e.time_us = esp_timer_get_time();
        queue_evt(&e);
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return fail("wifi.stop", "failed to stop wifi", err);
    }
    return NULL;
}

const char* drv_wifi_scan (const drv_wifi_scan_opts_t* opts) {
    if (s_scanning) {
        return drv_errf("wifi.scan : scan already in progress");
    }
    const char* ssid = NULL;
    int channel = 0;
    bool show_hidden = false;
    int max_aps = WIFI_SCAN_DEF_MAX;

    if (opts != NULL) {
        if (opts->ssid != NULL) {
            if (strlen(opts->ssid) > 32) {
                return drv_errf("wifi.scan : ssid must be at most 32 characters");
            }
            ssid = opts->ssid;
        }
        if (opts->channel < 0 || opts->channel > 14) {
            return drv_errf("wifi.scan : invalid Wi-Fi channel (1-14)");
        }
        channel = opts->channel;
        show_hidden = opts->show_hidden;
        if (opts->max_aps > 0) {
            max_aps = (opts->max_aps > DRV_WIFI_SCAN_MAX) ? DRV_WIFI_SCAN_MAX : opts->max_aps;
        }
    }

    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return fail("wifi.scan", "failed to initialize wifi", err);
    }
    if (ensure_sta_netif() == NULL) {
        return drv_errf("wifi.scan : failed to create STA netif");
    }

    wifi_mode_t mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if ((err = esp_wifi_set_mode(mode)) != ESP_OK) {
        return fail("wifi.scan", "failed to set wifi mode", err);
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        return fail("wifi.scan", "failed to start wifi", err);
    }
    radio_use(RADIO_WIFI);

    lock_take();
    s_started = true;
    if (s_state == ST_STOPPED) {
        s_state = ST_DISCONNECTED;
    }
    s_scan_max = max_aps;
    s_scanning = true;
    lock_give();

    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    uint8_t ssid_buf[33] = {0};
    if (ssid != NULL && ssid[0] != '\0') {
        memcpy(ssid_buf, ssid, strlen(ssid));
        sc.ssid = ssid_buf;
    }
    sc.channel = (uint8_t) channel;
    sc.show_hidden = show_hidden;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        lock_take();
        s_scanning = false;
        lock_give();
        return fail("wifi.scan", "esp_wifi_scan_start failed", err);
    }
    return NULL;
}

int drv_wifi_scan_result (int index, drv_wifi_ap_t* ap) {
    int ok = 0;
    lock_take();
    if (index >= 0 && index < s_scan_n) {
        *ap = s_scan_aps[index];
        ok = 1;
    }
    lock_give();
    return ok;
}

const char* drv_wifi_start_ap (const char* ssid, const char* password,
                               const drv_wifi_ap_opts_t* opts) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return drv_errf("wifi.start_ap : ssid must be 1 to 32 characters");
    }
    size_t plen = (password != NULL) ? strlen(password) : 0;
    if (plen > 0 && (plen < 8 || plen > 64)) {
        return drv_errf("wifi.start_ap : password must be 8 to 64 characters "
                        "(or empty for open AP)");
    }

    int channel = 1;
    int max_conn = 4;
    bool hidden = false;
    if (opts != NULL) {
        if (opts->channel != 0) {
            if (opts->channel < 1 || opts->channel > 13) {
                return drv_errf("wifi.start_ap : invalid AP channel (1-13)");
            }
            channel = opts->channel;
        }
        if (opts->max_conn != 0) {
            if (opts->max_conn < 1 || opts->max_conn > 10) {
                return drv_errf("wifi.start_ap : max_conn must be between 1 and 10");
            }
            max_conn = opts->max_conn;
        }
        hidden = opts->hidden;
    }

    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return fail("wifi.start_ap", "failed to initialize wifi", err);
    }
    if (ensure_ap_netif() == NULL) {
        return drv_errf("wifi.start_ap : failed to create AP netif");
    }

    wifi_mode_t mode = sta_in_use() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    if ((err = esp_wifi_set_mode(mode)) != ESP_OK) {
        return fail("wifi.start_ap", "failed to set wifi mode", err);
    }

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    size_t slen = strlen(ssid);
    memcpy(wcfg.ap.ssid, ssid, slen);
    wcfg.ap.ssid_len = (uint8_t) slen;
    wcfg.ap.channel = (uint8_t) channel;
    wcfg.ap.max_connection = (uint8_t) max_conn;
    wcfg.ap.ssid_hidden = hidden ? 1 : 0;
    if (plen > 0) {
        memcpy(wcfg.ap.password, password, plen);
        wcfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wcfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wcfg)) != ESP_OK) {
        return fail("wifi.start_ap", "failed to configure AP", err);
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        return fail("wifi.start_ap", "failed to start wifi", err);
    }
    radio_use(RADIO_WIFI);

    lock_take();
    s_started = true;
    s_ap_active = true;
    strcpy(s_ap_ssid, ssid);
    if (s_state == ST_STOPPED) {
        s_state = ST_DISCONNECTED;
    }
    lock_give();
    return NULL;
}

const char* drv_wifi_stop_ap (void) {
    if (!s_ap_active) {
        return NULL;
    }
    wifi_mode_t cur = WIFI_MODE_NULL;
    (void) esp_wifi_get_mode(&cur);
    esp_err_t err = ESP_OK;
    if (cur == WIFI_MODE_APSTA || sta_in_use()) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    } else {
        err = esp_wifi_stop();
        (void) esp_wifi_set_mode(WIFI_MODE_NULL);
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        return fail("wifi.stop_ap", "failed to stop AP", err);
    }
    ap_client_t gone[AP_CLIENTS_MAX];
    lock_take();
    s_ap_active = false;        // from now, the handler drops the AP events
    int ngone = clients_take(gone);
    s_ap_ssid[0] = '\0';
    bool sta_active = s_sta_active;
    if (cur != WIFI_MODE_APSTA && !s_sta_active) {
        s_started = false;
        s_state = ST_STOPPED;
    }
    lock_give();
    queue_clients_gone(gone, ngone);
    if (!sta_active) {
        (void) radio_release(RADIO_WIFI);  // ESP-NOW, if any, keeps it on
    }
    return NULL;
}

const char* drv_wifi_status (const char** state) {
    lock_take();
    *state = state_name(s_state);
    lock_give();
    return NULL;
}

const char* drv_wifi_get_mac (char out[18]) {
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            return fail("wifi.get_mac", "failed to get local MAC address", err);
        }
    }
    fmt_mac(mac, out);
    return NULL;
}

const char* drv_wifi_get_channel (int* channel) {
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_channel(&primary, &second);
    if (err != ESP_OK) {
        return fail("wifi.get_channel", "failed to get wifi channel", err);
    }
    *channel = primary;
    return NULL;
}

const char* drv_wifi_info (drv_wifi_info_t* info) {
    memset(info, 0, sizeof(*info));

    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK ||
        esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        fmt_mac(mac, info->mac);
    }
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK ||
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        fmt_mac(mac, info->ap_mac);
    }

    lock_take();
    info->state = state_name(s_state);
    info->ap_active = s_ap_active;
    strcpy(info->ssid, s_ssid);
    strcpy(info->bssid, s_bssid);
    strcpy(info->ip, s_ip);
    strcpy(info->netmask, s_netmask);
    strcpy(info->gateway, s_gateway);
    strcpy(info->ap_ssid, s_ap_ssid);
    info->channel = s_channel;
    wifi_st_t st = s_state;
    bool ap_on = s_ap_active;
    bool started = s_started;
    lock_give();

    if (st == ST_CONNECTED || st == ST_GOT_IP) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            info->rssi = rec.rssi;
            info->has_rssi = true;
            info->channel = rec.primary;
        }
    }
    if (info->channel == 0 && (ap_on || started)) {
        uint8_t primary = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary > 0) {
            info->channel = primary;
        }
    }
    if (ap_on) {
        esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip_info;
        if (ap != NULL && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, info->ap_ip, sizeof(info->ap_ip));
        }
    }
    return NULL;
}

bool drv_wifi_next (drv_wifi_evt_t* evt) {
    return s_queue != NULL && xQueueReceive(s_queue, evt, 0) == pdTRUE;
}
