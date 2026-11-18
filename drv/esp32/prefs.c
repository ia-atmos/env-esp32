// Preferences on the ESP32: drv/drv.h over NVS, one namespace open at a
// time. From main/preferences.c and lua/esp32/preferences.lua (the Lua
// version), with the Lua API taken out.

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "drv.h"

static nvs_handle_t s_handle;
static bool s_open = false;
static char* s_str = NULL;

static const char* check (const char* fn, const char* key) {
    if (!s_open) {
        return drv_errf("%s : no namespace is open (call preferences.open first)", fn);
    }
    size_t n = strlen(key);
    if (n<1 || n>15) {
        return drv_errf("%s : key length must be between 1 and 15 characters", fn);
    }
    return NULL;
}

static const char* commit (const char* fn, const char* key, bool yes) {
    if (!yes) {
        return NULL;
    }
    esp_err_t err = nvs_commit(s_handle);
    return (err == ESP_OK) ? NULL
         : drv_errf("%s : failed to commit '%s': %s", fn, key, esp_err_to_name(err));
}

const char* drv_prefs_open (const char* ns, bool readonly, const char* part) {
    size_t n = strlen(ns);
    if (n<1 || n>15) {
        return drv_errf("preferences.open : namespace length must be between 1 and 15 characters");
    }
    (void) drv_prefs_close();
    nvs_open_mode_t mode = readonly ? NVS_READONLY : NVS_READWRITE;
    esp_err_t err = (part != NULL)
        ? nvs_open_from_partition(part, ns, mode, &s_handle)
        : nvs_open(ns, mode, &s_handle);
    if (err != ESP_OK) {
        return drv_errf("preferences.open : failed to open namespace '%s': %s",
                        ns, esp_err_to_name(err));
    }
    s_open = true;
    return NULL;
}

const char* drv_prefs_close (void) {
    if (s_open) {
        (void) nvs_commit(s_handle);
        nvs_close(s_handle);
        s_open = false;
    }
    return NULL;
}

const char* drv_prefs_commit (void) {
    if (!s_open) {
        return drv_errf("preferences.commit : no namespace is open (call preferences.open first)");
    }
    esp_err_t err = nvs_commit(s_handle);
    return (err == ESP_OK) ? NULL
         : drv_errf("preferences.commit : failed to commit: %s", esp_err_to_name(err));
}

#define PUT(fn, what, call)                                                 \
    const char* e = check(fn, key);                                         \
    if (e != NULL) {                                                        \
        return e;                                                           \
    }                                                                       \
    esp_err_t err = (call);                                                 \
    if (err != ESP_OK) {                                                    \
        return drv_errf("%s : failed to write " what " '%s': %s",           \
                        fn, key, esp_err_to_name(err));                     \
    }                                                                       \
    return commit(fn, key, yes);

#define GET(fn, what, call)                                                 \
    *found = false;                                                         \
    const char* e = check(fn, key);                                         \
    if (e != NULL) {                                                        \
        return e;                                                           \
    }                                                                       \
    esp_err_t err = (call);                                                 \
    if (err == ESP_ERR_NVS_NOT_FOUND) {                                     \
        return NULL;                                                        \
    }                                                                       \
    if (err != ESP_OK) {                                                    \
        return drv_errf("%s : failed to read " what " '%s': %s",            \
                        fn, key, esp_err_to_name(err));                     \
    }                                                                       \
    *found = true;

const char* drv_prefs_put_int (const char* key, int32_t v, bool yes) {
    PUT("preferences.put_int", "int", nvs_set_i32(s_handle, key, v));
}

const char* drv_prefs_get_int (const char* key, bool* found, int32_t* v) {
    GET("preferences.get_int", "int", nvs_get_i32(s_handle, key, v));
    return NULL;
}

const char* drv_prefs_put_bool (const char* key, bool v, bool yes) {
    PUT("preferences.put_bool", "bool", nvs_set_u8(s_handle, key, v ? 1 : 0));
}

const char* drv_prefs_get_bool (const char* key, bool* found, bool* v) {
    uint8_t u = 0;
    GET("preferences.get_bool", "bool", nvs_get_u8(s_handle, key, &u));
    *v = (u != 0);
    return NULL;
}

const char* drv_prefs_put_str (const char* key, const char* v, bool yes) {
    PUT("preferences.put_string", "string", nvs_set_str(s_handle, key, v));
}

const char* drv_prefs_get_str (const char* key, bool* found, const char** v) {
    size_t n = 0;
    GET("preferences.get_string", "string", nvs_get_str(s_handle, key, NULL, &n));
    *found = false;
    char* buf = realloc(s_str, n);
    if (buf == NULL) {
        return drv_errf("preferences.get_string : out of memory reading '%s'", key);
    }
    s_str = buf;
    err = nvs_get_str(s_handle, key, s_str, &n);
    if (err != ESP_OK) {
        return drv_errf("preferences.get_string : failed to read string '%s': %s",
                        key, esp_err_to_name(err));
    }
    *found = true;
    *v = s_str;
    return NULL;
}

const char* drv_prefs_is_key (const char* key, bool* yes) {
    *yes = false;
    const char* e = check("preferences.is_key", key);
    if (e != NULL) {
        return e;
    }
    nvs_type_t type;
    *yes = (nvs_find_key(s_handle, key, &type) == ESP_OK);
    return NULL;
}

const char* drv_prefs_remove (const char* key, bool yes) {
    const char* e = check("preferences.remove", key);
    if (e != NULL) {
        return e;
    }
    esp_err_t err = nvs_erase_key(s_handle, key);
    if (err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) {
        return drv_errf("preferences.remove : failed to erase key '%s': %s",
                        key, esp_err_to_name(err));
    }
    return commit("preferences.remove", key, yes);
}

const char* drv_prefs_clear (bool yes) {
    const char* e = check("preferences.clear", "x");
    if (e != NULL) {
        return e;
    }
    esp_err_t err = nvs_erase_all(s_handle);
    if (err != ESP_OK) {
        return drv_errf("preferences.clear : failed to clear namespace: %s",
                        esp_err_to_name(err));
    }
    return commit("preferences.clear", "*", yes);
}
