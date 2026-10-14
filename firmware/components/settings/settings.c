#include "settings.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NS "ms"
#define KEY_WIFI "wifi"     /* blob: settings_wifi_t[n], newest first */

/* Namespaces owned by other components that a factory reset must wipe as well. */
static const char *const s_reset_namespaces[] = {
    NS,
    "spotify",   /* spotify_connect: reusable Spotify credentials */
    "sp_api",    /* spotify_api: refresh token, link date         */
    "nfc_tags",  /* app: per-tag options keyed by UID             */
    "net",       /* net: anything the network component persists  */
};

static SemaphoreHandle_t s_lock;

static esp_err_t open_ns(nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_open(NS, mode, h);
}

esp_err_t settings_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t settings_get_str(const char *key, char *out, size_t cap)
{
    if (!key || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = cap;
        err = nvs_get_str(h, key, out, &len);
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

static esp_err_t write_locked(const char *key, const char *str, const uint32_t *u32, bool erase)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (erase) {
            err = nvs_erase_key(h, key);
        } else if (str) {
            err = nvs_set_str(h, key, str);
        } else {
            err = nvs_set_u32(h, key, *u32);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    }
    return err;
}

esp_err_t settings_set_str(const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_locked(key, value, NULL, false);
}

esp_err_t settings_get_u32(const char *key, uint32_t *out)
{
    if (!key || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, key, out);
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t settings_set_u32(const char *key, uint32_t value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_locked(key, NULL, &value, false);
}

uint32_t settings_get_u32_or(const char *key, uint32_t fallback)
{
    uint32_t v;
    return settings_get_u32(key, &v) == ESP_OK ? v : fallback;
}

esp_err_t settings_erase(const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_locked(key, NULL, NULL, true);
}

/* ---- WiFi list ----------------------------------------------------------- */

static int wifi_load_locked(settings_wifi_t *list)
{
    nvs_handle_t h;
    if (open_ns(NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    size_t len = sizeof(settings_wifi_t) * SETTINGS_WIFI_MAX;
    esp_err_t err = nvs_get_blob(h, KEY_WIFI, list, &len);
    nvs_close(h);
    if (err != ESP_OK || len % sizeof(settings_wifi_t) != 0) {
        return 0;
    }
    int n = (int)(len / sizeof(settings_wifi_t));
    for (int i = 0; i < n; i++) {   /* never trust stored strings to be terminated */
        list[i].ssid[sizeof(list[i].ssid) - 1] = '\0';
        list[i].password[sizeof(list[i].password) - 1] = '\0';
    }
    return n;
}

static esp_err_t wifi_store_locked(const settings_wifi_t *list, int n)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = n > 0 ? nvs_set_blob(h, KEY_WIFI, list, sizeof(settings_wifi_t) * (size_t)n)
                : nvs_erase_key(h, KEY_WIFI);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static int wifi_find(const settings_wifi_t *list, int n, const char *ssid)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

int settings_wifi_list(settings_wifi_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    settings_wifi_t list[SETTINGS_WIFI_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = wifi_load_locked(list);
    xSemaphoreGive(s_lock);
    if (n > max) {
        n = max;
    }
    memcpy(out, list, sizeof(settings_wifi_t) * (size_t)n);
    return n;
}

/* Puts `entry` at index 0 and keeps the remaining entries in order. */
static esp_err_t wifi_put_front_locked(settings_wifi_t *list, int n, int existing,
                                       const settings_wifi_t *entry)
{
    settings_wifi_t out[SETTINGS_WIFI_MAX];
    int m = 0;
    out[m++] = *entry;
    for (int i = 0; i < n && m < SETTINGS_WIFI_MAX; i++) {
        if (i != existing) {
            out[m++] = list[i];
        }
    }
    return wifi_store_locked(out, m);
}

esp_err_t settings_wifi_add(const char *ssid, const char *password)
{
    if (!ssid || !password || ssid[0] == '\0' || strlen(ssid) > 32 || strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_wifi_t entry = {0};
    strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    strncpy(entry.password, password, sizeof(entry.password) - 1);

    settings_wifi_t list[SETTINGS_WIFI_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = wifi_load_locked(list);
    esp_err_t err = wifi_put_front_locked(list, n, wifi_find(list, n, ssid), &entry);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_wifi_mark_used(const char *ssid)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_wifi_t list[SETTINGS_WIFI_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = wifi_load_locked(list);
    int idx = wifi_find(list, n, ssid);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (idx == 0) {
        err = ESP_OK;
    } else if (idx > 0) {
        settings_wifi_t entry = list[idx];
        err = wifi_put_front_locked(list, n, idx, &entry);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_wifi_remove(const char *ssid)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_wifi_t list[SETTINGS_WIFI_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = wifi_load_locked(list);
    int idx = wifi_find(list, n, ssid);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (idx >= 0) {
        memmove(&list[idx], &list[idx + 1], sizeof(settings_wifi_t) * (size_t)(n - idx - 1));
        err = wifi_store_locked(list, n - 1);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_factory_reset(void)
{
    esp_err_t first_err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(s_reset_namespaces) / sizeof(s_reset_namespaces[0]); i++) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(s_reset_namespaces[i], NVS_READWRITE, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;   /* namespace never written */
        }
        if (err == ESP_OK) {
            err = nvs_erase_all(h);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erasing namespace %s failed: %s", s_reset_namespaces[i], esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return first_err;
}
