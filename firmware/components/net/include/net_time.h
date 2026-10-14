/*
 * Wall-clock time: SNTP plus the local time zone (default Europe/Berlin,
 * CONFIG_MS_NET_TZ = "CET-1CEST,M3.5.0,M10.5.0/3").
 *
 * net_time_start() may be called before the network is up (SNTP keeps
 * retrying in the background); after it, localtime_r() gives German time
 * once net_time_is_set() is true. TLS does not need the time (certificate
 * dates are not checked by the ESP-IDF default mbedTLS configuration).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Set TZ and start SNTP (CONFIG_MS_NET_SNTP_SERVER). Idempotent. Requires esp_netif_init(). */
esp_err_t net_time_start(void);

/** The clock was set by SNTP at least once since boot. */
bool net_time_is_set(void);

#ifdef __cplusplus
}
#endif
