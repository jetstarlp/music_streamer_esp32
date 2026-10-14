/*
 * Application state shared between app_main, the console and the status LED.
 * (Bench firmware; the real "app" state machine of work package P7 replaces
 * this.)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SPOTIFY_NOT_CONFIGURED = 0, /* no client id / secret                  */
    APP_SPOTIFY_WAITING_NETWORK,    /* configured, network not up yet         */
    APP_SPOTIFY_START_FAILED,       /* spotify_connect_start() returned error */
    APP_SPOTIFY_CONNECTING,         /* started, logging in                    */
    APP_SPOTIFY_WAITING_LOGIN,      /* Zeroconf: pick the device in the app   */
    APP_SPOTIFY_LOGGED_IN,
} app_spotify_state_t;

typedef struct {
    app_spotify_state_t state;
    bool started;                   /* spotify_connect_start() succeeded      */
    bool stored_credentials;        /* LOGGED_IN via stored credentials       */
    char user[64];                  /* Spotify user name, "" if unknown       */
    char track[160];                /* "artist - title" of the audible track  */
} app_spotify_info_t;

void app_get_spotify_info(app_spotify_info_t *out);
const char *app_spotify_state_name(app_spotify_state_t state);

/** Spotify plays on this device right now (active and not paused). */
bool app_spotify_playing(void);

/** Network interface has an IP address (WiFi, or emulated Ethernet in QEMU). */
bool app_network_up(void);

/** IPv4 address of the default interface in network byte order, 0 if none. */
uint32_t app_network_ip(void);

/** The Spotify credentials changed: try to start Spotify Connect if it is not running. */
void app_spotify_settings_changed(void);

#ifdef __cplusplus
}
#endif
