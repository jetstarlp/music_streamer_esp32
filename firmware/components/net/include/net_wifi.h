/*
 * WiFi station with up to SETTINGS_WIFI_MAX stored networks (formerly
 * main/wifi_sta.c of the bench firmware; the connection strategy that was
 * tested on hardware is unchanged).
 *
 * A background task walks through the candidate networks (by default the ones
 * stored in NVS, most recently used first; the application may add more, e.g.
 * a build-time network) and stays on the first one that hands out an IP
 * address; that network is then moved to the front of the stored list
 * (settings_wifi_mark_used). Every attempt scans all channels and picks the
 * strongest access point (mesh systems, repeaters), the driver retries each
 * access point 3 times. When the connection drops the task retries the same
 * network 3 times before walking the list again; after a round in which every
 * network failed it pauses 1, 3, 5, 10, then 15 s. Modem power save is off
 * (it causes audible drop-outs).
 *
 * Setup hotspot (net_portal.h): it starts automatically when no network is
 * stored, or without an IP address for CONFIG_MS_NET_PORTAL_AUTO_S seconds
 * (60 s; after boot or after losing the connection). While it runs, WiFi is
 * in AP+STA mode and the task keeps trying the stored networks; while a phone
 * is connected to the hotspot it only looks on the hotspot's own channel, so
 * the phone does not lose the hotspot.
 *
 * Thread safety: all functions may be called from any task; none blocks for
 * long except net_wifi_wait_connected().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_WIFI_STOPPED = 0,       /* net_wifi_start() not called           */
    NET_WIFI_NO_NETWORKS,       /* nothing configured                    */
    NET_WIFI_CONNECTING,        /* trying `ssid`                         */
    NET_WIFI_CONNECTED,         /* associated with `ssid`, IP assigned   */
    NET_WIFI_WAITING,           /* all networks failed, pausing          */
} net_wifi_state_t;

typedef struct {
    net_wifi_state_t state;
    char     ssid[33];          /* current/last attempted network        */
    uint32_t ip;                /* IPv4, network byte order; 0 if none   */
    int8_t   rssi;              /* dBm, only valid while connected       */
    uint8_t  channel;           /* only valid while connected            */
    uint8_t  last_reason;       /* wifi_err_reason_t of the last disconnect */
} net_wifi_info_t;

typedef struct {
    /** Fills `out` with the networks to try, in order; returns the count.
     *  NULL: settings_wifi_list(). Called from the WiFi task. */
    int (*list_networks)(settings_wifi_t *out, int max);
    /** Spotify Connect name, shown on the portal's success page (copied). */
    const char *device_name;
} net_wifi_config_t;

/**
 * Start WiFi in station mode and the connection task (4 kB internal stack).
 * Requires settings_init(), esp_netif_init() and the default event loop.
 * `cfg` may be NULL.
 */
esp_err_t net_wifi_start(const net_wifi_config_t *cfg);

/** Block until an IP address was obtained. @return false on timeout. */
bool net_wifi_wait_connected(TickType_t timeout);

/** The station has an IP address (not the setup hotspot). */
bool net_wifi_is_connected(void);

void net_wifi_get_info(net_wifi_info_t *out);

/** The list of networks changed: retry now instead of after the pause. */
void net_wifi_rescan(void);

/** English, for logs and the console. */
const char *net_wifi_state_name(net_wifi_state_t state);

/** German, user-facing explanation of a wifi_err_reason_t, e.g. "Falsches Passwort? ...". */
const char *net_wifi_reason_text_de(uint8_t reason);

#ifdef __cplusplus
}
#endif
