#include "app_settings.h"

#include <stdio.h>

#include "bench_util.h"
#include "sdkconfig.h"

/* Build-time fallbacks from Kconfig. Empty in every image that is handed out
 * (sdkconfig.xiao clears them, tools/build_dist.sh checks it). */
static const char *const s_build_ssid = CONFIG_MS_WIFI_SSID;
static const char *const s_build_password = CONFIG_MS_WIFI_PASSWORD;
static const char *const s_build_device_name = CONFIG_MS_DEVICE_NAME;
static const char *const s_build_client_id = CONFIG_MS_SPOTIFY_CLIENT_ID;
static const char *const s_build_client_secret = CONFIG_MS_SPOTIFY_CLIENT_SECRET;

static void copy_str(char *out, size_t cap, const char *src)
{
    if (cap > 0) {
        snprintf(out, cap, "%s", src);
    }
}

app_setting_source_t app_settings_device_name(char *out, size_t cap)
{
    if (settings_get_str(SETTINGS_KEY_DEVICE_NAME, out, cap) == ESP_OK && out[0] != '\0') {
        return APP_SETTING_FROM_NVS;
    }
    if (s_build_device_name[0] != '\0') {
        copy_str(out, cap, s_build_device_name);
        return APP_SETTING_FROM_BUILD;
    }
    copy_str(out, cap, APP_DEFAULT_DEVICE_NAME);
    return APP_SETTING_FROM_BUILD;
}

app_setting_source_t app_settings_spotify_credentials(char *client_id, size_t id_cap,
                                                      char *client_secret, size_t secret_cap)
{
    if (settings_get_str(SETTINGS_KEY_SP_CLIENT_ID, client_id, id_cap) == ESP_OK &&
        settings_get_str(SETTINGS_KEY_SP_CLIENT_SECRET, client_secret, secret_cap) == ESP_OK &&
        client_id[0] != '\0' && client_secret[0] != '\0') {
        return APP_SETTING_FROM_NVS;
    }
    if (s_build_client_id[0] != '\0' && s_build_client_secret[0] != '\0') {
        copy_str(client_id, id_cap, s_build_client_id);
        copy_str(client_secret, secret_cap, s_build_client_secret);
        return APP_SETTING_FROM_BUILD;
    }
    copy_str(client_id, id_cap, "");
    copy_str(client_secret, secret_cap, "");
    return APP_SETTING_UNSET;
}

int app_settings_spotify_bitrate(void)
{
    uint32_t v = settings_get_u32_or(SETTINGS_KEY_SP_BITRATE, 0);
    if (v == 96 || v == 160 || v == 320) {
        return (int)v;
    }
    return CONFIG_MS_SPOTIFY_BITRATE;
}

int app_settings_wifi_list(settings_wifi_t *out, int max)
{
    settings_wifi_t stored[SETTINGS_WIFI_MAX];
    int n = settings_wifi_list(stored, SETTINGS_WIFI_MAX);
    return bench_merge_wifi(stored, n, s_build_ssid, s_build_password, out, max);
}

const char *app_setting_source_name(app_setting_source_t src)
{
    switch (src) {
    case APP_SETTING_FROM_NVS:
        return "stored on device";
    case APP_SETTING_FROM_BUILD:
        return "build-time default";
    case APP_SETTING_UNSET:
    default:
        return "not set";
    }
}
