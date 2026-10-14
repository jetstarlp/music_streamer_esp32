/* SNTP and time zone, see include/net_time.h. */
#include "net_time.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

static const char *TAG = "net_time";

static bool s_started;
static volatile bool s_synced;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    if (!s_synced) {
        s_synced = true;
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
        ESP_LOGI(TAG, "clock set: %s", buf);
    }
}

esp_err_t net_time_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    setenv("TZ", CONFIG_MS_NET_TZ, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MS_NET_SNTP_SERVER);
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_started = true;
    } else {
        ESP_LOGW(TAG, "SNTP: %s", esp_err_to_name(err));
    }
    return err;
}

bool net_time_is_set(void)
{
    return s_synced;
}
