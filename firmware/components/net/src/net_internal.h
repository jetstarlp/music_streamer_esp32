/* Private interfaces between net_wifi.c, net_portal.c and net_ota.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "net_pure.h"

/* Reason codes of net_attempt_t besides wifi_err_reason_t. */
#define NET_REASON_UNAVAILABLE  0       /* WiFi not started (QEMU build)      */
#define NET_REASON_NO_IP        250     /* associated, but no DHCP lease      */

typedef enum {
    NET_ATTEMPT_NONE = 0,
    NET_ATTEMPT_RUNNING,
    NET_ATTEMPT_OK,
    NET_ATTEMPT_FAILED,
} net_attempt_state_t;

/* Outcome of the last network entered on the portal page. */
typedef struct {
    net_attempt_state_t state;
    char     ssid[33];
    uint8_t  reason;        /* FAILED: wifi_err_reason_t or NET_REASON_* */
    uint32_t ip;            /* OK: IPv4, network byte order             */
} net_attempt_t;

/* ---- net_wifi.c ---------------------------------------------------------------- */

/** Try this network next (aborting a background attempt); stored on success. */
esp_err_t net_wifi_request(const char *ssid, const char *password);
void      net_wifi_get_attempt(net_attempt_t *out);
void      net_wifi_clear_attempt(void);

/**
 * Scan for networks (blocking, ~2 s for all channels, ~150 ms for one).
 * `channel` 0 = all channels. A running background connection attempt is
 * aborted for it; a requested one is waited for (up to `wait`). The result is
 * normalized (net_scan_normalize).
 */
esp_err_t net_wifi_scan(uint8_t channel, net_ap_info_t *out, size_t max, size_t *n, TickType_t wait);

/**
 * Exclusive use of the radio (mode changes, scans, connection attempts). A
 * background connection attempt that holds it gives it up within ~250 ms.
 */
bool net_wifi_radio_lock(TickType_t timeout);
void net_wifi_radio_unlock(void);

bool        net_wifi_started(void);
const char *net_wifi_device_name(void);
/** Stored network names (for the hotspot channel choice); returns the count. */
size_t      net_wifi_known_ssids(char (*out)[33], size_t max);

/* ---- net_portal.c ---------------------------------------------------------------- */

void net_portal_auto_start(void);
/** A phone is on the hotspot: background attempts must not leave its channel. */
bool net_portal_wants_quiet(void);
/** Channel the hotspot runs on (0 if not active). */
uint8_t net_portal_channel(void);
/** Called by the WiFi task when a network entered on the page succeeded/failed. */
void net_portal_attempt_done(const net_attempt_t *at);

/* ---- net_ota.c ------------------------------------------------------------------- */

/** The running firmware waits for net_ota_mark_valid() (a restart would roll it back). */
bool net_ota_pending_verify(void);
