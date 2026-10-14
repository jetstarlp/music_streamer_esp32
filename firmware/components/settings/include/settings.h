/*
 * settings - persistent device settings in NVS (namespace "ms").
 *
 * Single place for everything the user configures at runtime: WiFi networks,
 * Spotify developer credentials, device name, display and tag behaviour.
 * Other components store their own state in their own NVS namespaces; the
 * namespaces erased by settings_factory_reset() are listed in settings.c.
 *
 * All functions are thread-safe. settings_init() must be called once before
 * anything else (it also initialises the NVS flash partition).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Well-known keys (NVS keys are limited to 15 characters). */
#define SETTINGS_KEY_DEVICE_NAME        "dev_name"      /* str, Spotify Connect name     */
#define SETTINGS_KEY_SP_CLIENT_ID       "sp_cid"        /* str, Spotify developer app    */
#define SETTINGS_KEY_SP_CLIENT_SECRET   "sp_csec"       /* str                           */
#define SETTINGS_KEY_SP_BITRATE         "sp_rate"       /* u32, 96 / 160 / 320           */
#define SETTINGS_KEY_BRIGHTNESS         "brightness"    /* u32, 0..100 %                 */
#define SETTINGS_KEY_LANGUAGE           "lang"          /* str, "de" / "en"              */
#define SETTINGS_KEY_TAG_REMOVE_ACTION  "tag_remove"    /* u32, see settings_tag_remove_t */
#define SETTINGS_KEY_VOLUME             "volume"        /* u32, 0..65535, last Spotify volume */
#define SETTINGS_KEY_SAVER_MINUTES      "saver_min"     /* u32, screensaver after 1/5/15 min */
#define SETTINGS_KEY_RST_BROWNOUT       "rst_bo"        /* u32, brownout resets so far   */
#define SETTINGS_KEY_RST_CRASH          "rst_crash"     /* u32, panic/watchdog resets    */
#define SETTINGS_KEY_OTA_URL            "ota_url"       /* str, OTA manifest URL (overrides Kconfig) */
#define SETTINGS_KEY_OTA_BAD_VERSION    "ota_bad"       /* str, firmware version that was rolled back */

typedef enum {
    SETTINGS_TAG_REMOVE_PAUSE = 0,      /* default: pause when the tag is taken away */
    SETTINGS_TAG_REMOVE_KEEP_PLAYING = 1,
    SETTINGS_TAG_REMOVE_STOP_AFTER_TRACK = 2,
} settings_tag_remove_t;

#define SETTINGS_WIFI_MAX 5

typedef struct {
    char ssid[33];      /* 32 bytes + NUL */
    char password[65];  /* 64 bytes + NUL (WPA2 PSK as hex is 64 chars) */
} settings_wifi_t;

/** Initialise NVS (erasing it only if the partition layout is incompatible). */
esp_err_t settings_init(void);

/** ESP_ERR_NOT_FOUND if unset, ESP_ERR_NVS_INVALID_LENGTH if cap is too small. */
esp_err_t settings_get_str(const char *key, char *out, size_t cap);
esp_err_t settings_set_str(const char *key, const char *value);
esp_err_t settings_get_u32(const char *key, uint32_t *out);
esp_err_t settings_set_u32(const char *key, uint32_t value);
/** Returns `fallback` when the key is unset or unreadable. */
uint32_t  settings_get_u32_or(const char *key, uint32_t fallback);
esp_err_t settings_erase(const char *key);

/** Copies up to `max` stored networks, most recently added first; returns the count. */
int       settings_wifi_list(settings_wifi_t *out, int max);
/** Adds or updates a network (matched by SSID). When full, the oldest entry is dropped. */
esp_err_t settings_wifi_add(const char *ssid, const char *password);
/** ESP_ERR_NOT_FOUND if no such SSID is stored. */
esp_err_t settings_wifi_remove(const char *ssid);
/** Moves an existing entry to the front (call after a successful connection). */
esp_err_t settings_wifi_mark_used(const char *ssid);

/** Erases all user data (this namespace and the other components' namespaces). */
esp_err_t settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
