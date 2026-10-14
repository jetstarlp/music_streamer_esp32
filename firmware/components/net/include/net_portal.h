/*
 * WiFi setup portal ("Einrichtungs-Hotspot").
 *
 * An open access point (CONFIG_MS_NET_PORTAL_SSID, default "Music-Streamer",
 * optionally with "-XXXX" from the MAC address) at 192.168.4.1 with a captive
 * DNS server (every name resolves to 192.168.4.1) and a small German web page:
 * networks in range (strongest first, lock icon for encrypted ones), network
 * name, password, "Verbinden". The phones' captive-portal checks (Android
 * /generate_204, Apple /hotspot-detect.html, Windows /connecttest.txt, ...)
 * get a redirect to the page, so the phone shows "Im Netzwerk anmelden" and
 * opens the page by itself; DHCP option 114 announces the page as well.
 *
 * Open on purpose: the recipient only has to tap the network name or scan the
 * WiFi QR code on the display (net_portal_get_qr_payload). The page answers
 * only on the hotspot, never on the home network, and the hotspot runs only
 * while the device has no working WiFi or after an explicit request.
 *
 * Flow: the network entered on the page is tried at once (AP+STA). On success
 * it is stored (settings_wifi_add), the page shows "Verbunden! Das Gerät
 * startet neu …" and the device restarts 8 s later
 * (CONFIG_MS_NET_PORTAL_RESTART_AFTER_SETUP; not while a freshly installed
 * update waits for confirmation, a restart would roll it back - then only the
 * hotspot stops after 30 s). On failure the form comes back with the reason
 * ("Falsches Passwort? ..."); nothing is stored, so a typo never pushes a
 * working network out of the list of five.
 *
 * The hotspot also stops
 *   - when it was started automatically, as soon as a stored network is
 *     connected and no phone is on the hotspot,
 *   - when it was started by request, after 15 minutes without any phone,
 *   - on net_portal_stop().
 *
 * The hotspot uses the channel of the strongest stored (or, if none is in
 * range, the strongest) network, so that it does not have to change its
 * channel - and lose the phone - when the device connects.
 *
 * QEMU test build (no WiFi): net_portal_start() runs the page and the DNS
 * responder on the emulated Ethernet interface, for tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_PORTAL_IP   "192.168.4.1"
#define NET_PORTAL_URL  "http://192.168.4.1/"

typedef enum {
    NET_PORTAL_EVT_STARTED,         /* hotspot is up (ssid, requested)                 */
    NET_PORTAL_EVT_STOPPED,         /* hotspot is down again                           */
    NET_PORTAL_EVT_CLIENT_JOINED,   /* a phone joined the hotspot (clients)            */
    NET_PORTAL_EVT_CLIENT_LEFT,     /* a phone left (clients)                          */
    NET_PORTAL_EVT_CREDENTIALS,     /* a network was entered on the page (ssid)        */
    NET_PORTAL_EVT_CONNECT_OK,      /* ... and connected (ssid, ip); now stored        */
    NET_PORTAL_EVT_CONNECT_FAILED,  /* ... and failed (ssid, reason)                   */
    NET_PORTAL_EVT_RESTARTING,      /* the device restarts in a moment: mute the audio */
} net_portal_event_id_t;

typedef struct {
    net_portal_event_id_t id;
    char     ssid[33];      /* STARTED: hotspot name; CREDENTIALS/CONNECT_*: entered network */
    bool     requested;     /* STARTED: by net_portal_start() rather than automatically      */
    int      clients;       /* phones on the hotspot after the event                         */
    uint8_t  reason;        /* CONNECT_FAILED: wifi_err_reason_t, see net_wifi_reason_text_de() */
    uint32_t ip;            /* CONNECT_OK: IPv4 address in the home network, network order    */
} net_portal_event_t;

/**
 * Event callback. Runs on the portal task, the WiFi task or the default event
 * loop task: copy what you need and return, never block.
 */
typedef void (*net_portal_event_cb_t)(const net_portal_event_t *ev, void *ctx);

void net_portal_set_event_cb(net_portal_event_cb_t cb, void *ctx);

/**
 * Start the setup hotspot now (no effect if it already runs; a running
 * automatic portal becomes a requested one). Returns at once, the hotspot is
 * up about a second later (NET_PORTAL_EVT_STARTED). Requires net_wifi_start()
 * on the device. Uses ~12 kB of internal RAM while it runs (two tasks, AP
 * interface): do not start it during playback.
 */
esp_err_t net_portal_start(void);

/** Stop the hotspot (asynchronous, takes up to a second). */
void net_portal_stop(void);

bool net_portal_is_active(void);

/** Number of phones connected to the hotspot. */
int net_portal_client_count(void);

/** Name of the hotspot, e.g. "Music-Streamer". */
void net_portal_get_ssid(char *out, size_t cap);

/**
 * WiFi QR code payload for the hotspot, "WIFI:T:nopass;S:Music-Streamer;;"
 * (scanning it with the phone camera joins the hotspot).
 * @return length, 0 if `cap` is too small
 */
size_t net_portal_get_qr_payload(char *out, size_t cap);

/** URL of the setup page (text for the display, or a second QR code): NET_PORTAL_URL. */
const char *net_portal_get_url(void);

#ifdef __cplusplus
}
#endif
