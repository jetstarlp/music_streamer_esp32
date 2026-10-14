/*
 * Firmware updates over the internet (OTA) with rollback.
 *
 * A manifest (JSON over HTTPS; server certificate checked against the ESP-IDF
 * root certificate bundle, so GitHub and GitHub Pages work) names the newest
 * version:
 *
 *   {"version":"1.2.0", "url":"https://.../ms-waveshare-1.2.0.bin",
 *    "sha256":"<64 hex digits>", "notes":"...",
 *    "size":1480000, "min_version":"1.0.0"}          (size, min_version optional)
 *
 * "url" may be relative to the manifest URL. Redirects (GitHub release assets
 * -> objects.githubusercontent.com) are followed. "sha256" is the SHA-256 of
 * the application image file as served (build/music_streamer.bin, not the
 * merged USB image). Publishing: docs/NET.md, tools/make_ota_release.sh.
 *
 * Install: esp_https_ota writes the image into the inactive OTA slot; before
 * the slot is made bootable the written bytes are read back and their SHA-256
 * is compared with the manifest, and the image's own project name and version
 * must match. Then the device restarts (at once after net_ota_install(); a
 * busy callback can postpone it until nothing plays).
 *
 * Update rule (net_ota_decide in src/pure): install only if the manifest
 * version is strictly newer (semver) than the running version
 * (firmware/version.txt) and the running version is >= "min_version".
 *
 * Schedule: first check CONFIG_MS_OTA_FIRST_CHECK_S (120 s) after the network
 * came up, then every CONFIG_MS_OTA_CHECK_INTERVAL_H (24 h); after a failed
 * check one hour later. Scheduled checks only *report* a newer version
 * (state NET_OTA_AVAILABLE + event); the application asks the user and calls
 * net_ota_install(). (CONFIG_MS_OTA_AUTO_INSTALL=y installs by itself when
 * nothing plays.) Scheduled checks wait while the busy callback returns true.
 *
 * Rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): after an update the new
 * firmware starts in "pending verify" state. THE APPLICATION MUST CALL
 * net_ota_mark_valid() once it works (WiFi connected and Spotify logged in or
 * waiting for a login). If it does not do so within
 * CONFIG_MS_OTA_VERIFY_TIMEOUT_S (10 min), or the new firmware crashes or
 * restarts before, the bootloader starts the previous firmware again. That
 * version is then remembered (status.rolled_back_version) and not installed
 * automatically a second time.
 *
 * The manifest URL comes from CONFIG_MS_OTA_MANIFEST_URL (argument of
 * net_ota_start) and can be overridden at runtime (net_ota_set_url, stored in
 * NVS under SETTINGS_KEY_OTA_URL). Empty: OTA disabled.
 *
 * RAM: the worker task (8 kB internal stack) exists only while a check or an
 * install runs; TLS and download buffers are in PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_OTA_DISABLED = 0,       /* no manifest URL configured                       */
    NET_OTA_IDLE,               /* waiting for the next check                       */
    NET_OTA_CHECKING,           /* downloading the manifest                         */
    NET_OTA_UP_TO_DATE,         /* last check: nothing newer                        */
    NET_OTA_AVAILABLE,          /* newer version known, not (yet) installed         */
    NET_OTA_DOWNLOADING,        /* writing the new image, see progress              */
    NET_OTA_VERIFYING,          /* checking SHA-256 of the written image            */
    NET_OTA_READY,              /* installed, restarts when nothing plays           */
    NET_OTA_REBOOTING,          /* the device restarts in a moment: mute the audio  */
    NET_OTA_FAILED,             /* last check or install failed, see last_error     */
} net_ota_state_t;

typedef struct {
    net_ota_state_t state;
    char     current_version[32];
    char     available_version[32]; /* "" until a manifest was read               */
    char     notes[160];            /* release notes from the manifest (UTF-8)    */
    uint8_t  progress;              /* 0..100 while DOWNLOADING                   */
    char     last_error[96];        /* English, "" if none                        */
    uint32_t last_check_age_s;      /* since the last successful check, UINT32_MAX = never */
    bool     pending_verify;        /* running image not confirmed yet (rollback armed) */
    bool     rolled_back;           /* an update was rolled back at some point    */
    char     rolled_back_version[32];
} net_ota_status_t;

typedef void (*net_ota_event_cb_t)(const net_ota_status_t *status, void *ctx);

/**
 * Start the OTA scheduler (an esp_timer). Requires settings_init() and
 * esp_netif_init(). Also detects a pending verification or an earlier
 * rollback of the running image (logged, see net_ota_get_status()).
 *
 * @param default_manifest_url  CONFIG_MS_OTA_MANIFEST_URL; NULL/"" = none
 */
esp_err_t net_ota_start(const char *default_manifest_url);

/**
 * Check now, only report (state UP_TO_DATE / AVAILABLE / FAILED).
 * Asynchronous: follow the state with net_ota_get_status() or the event cb.
 * @return ESP_ERR_INVALID_STATE while a check/install runs or OTA is disabled
 */
esp_err_t net_ota_check_now(void);

/**
 * Check and, if the manifest offers a newer version, download, verify, and
 * restart into it (regardless of the busy callback: the user asked for it).
 * Asynchronous like net_ota_check_now().
 */
esp_err_t net_ota_install(void);

void net_ota_get_status(net_ota_status_t *out);

/** Called on every state change and every 5 % of download progress, from the
 *  OTA task or the esp_timer task: copy what you need, do not block. */
void net_ota_set_event_cb(net_ota_event_cb_t cb, void *ctx);

/** `busy` returns true while a scheduled check or restart would disturb (music
 *  plays). Called from the esp_timer task: must not block. NULL: never busy. */
void net_ota_set_busy_cb(bool (*busy)(void));

/** Override the manifest URL (stored in NVS). NULL or "" removes the override. */
esp_err_t net_ota_set_url(const char *url);

/** The manifest URL in use; *from_nvs tells whether it is the NVS override. */
void net_ota_get_url(char *out, size_t cap, bool *from_nvs);

/**
 * Confirm that the running firmware works: cancels the pending rollback.
 * Cheap and idempotent; the application calls it once WiFi is connected and
 * Spotify Connect is logged in (or waits for a login from the app).
 */
esp_err_t net_ota_mark_valid(void);

const char *net_ota_state_name(net_ota_state_t state);

#ifdef __cplusplus
}
#endif
