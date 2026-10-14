/*
 * Runtime settings of the application.
 *
 * Every getter reads the settings component (NVS) first and falls back to the
 * compile-time value from Kconfig (menuconfig -> "Music Streamer", or
 * sdkconfig.secrets during development). Images that are handed out are built
 * with empty Kconfig credentials; the user then enters WiFi and Spotify data
 * on the serial console, which stores them in NVS.
 *
 * All functions copy into caller-provided buffers, because NVS values can
 * change at runtime (console commands). Thread-safe (the settings component
 * serialises NVS access). settings_init() must have been called.
 */
#pragma once

#include <stddef.h>

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SETTING_UNSET = 0,      /* neither NVS nor Kconfig has a value */
    APP_SETTING_FROM_NVS,       /* entered at runtime (console)         */
    APP_SETTING_FROM_BUILD,     /* Kconfig / sdkconfig.secrets          */
} app_setting_source_t;

#define APP_DEVICE_NAME_MAX     64      /* bytes including NUL (mDNS limit 63) */
#define APP_CREDENTIAL_MAX      65      /* bytes including NUL                  */
#define APP_DEFAULT_DEVICE_NAME "Music Streamer"

/** Spotify Connect name; never empty (default "Music Streamer"). */
app_setting_source_t app_settings_device_name(char *out, size_t cap);

/**
 * Spotify developer app credentials. Id and secret always come from the same
 * source: NVS when both are stored there, else Kconfig when both are set
 * there, else APP_SETTING_UNSET with both outputs "".
 */
app_setting_source_t app_settings_spotify_credentials(char *client_id, size_t id_cap,
                                                      char *client_secret, size_t secret_cap);

/** 96, 160 or 320 (kbit/s): NVS value if valid, else Kconfig. */
int app_settings_spotify_bitrate(void);

/**
 * Networks to try, in order: the ones stored in NVS (most recently used
 * first), then the Kconfig network if it is set and not stored already.
 * @return number of entries written (at most max).
 */
int app_settings_wifi_list(settings_wifi_t *out, int max);

const char *app_setting_source_name(app_setting_source_t src);

#ifdef __cplusplus
}
#endif
