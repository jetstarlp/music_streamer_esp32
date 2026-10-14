/*
 * spotify_connect - Spotify Connect receiver (wrapper around cspot + bell).
 *
 * The device shows up in the Spotify app as a speaker. Audio is decoded on the
 * ESP32 (Ogg Vorbis, 44.1 kHz/16 bit stereo) and pushed into audio_out.
 *
 * Login
 * -----
 * 1. First use ("Zeroconf"): the device advertises _spotify-connect._tcp via
 *    mDNS and serves a tiny HTTP endpoint. When the user picks the device in
 *    the Spotify app (same WiFi), the app posts an encrypted login blob.
 * 2. After a successful login Spotify hands out reusable credentials. They are
 *    stored in NVS (namespace "spotify", key "credentials") and used for all
 *    later boots, so the device is logged in - and remotely controllable via
 *    the Web API - without anybody touching the app first.
 *
 * Threading
 * ---------
 * All functions may be called from any task. The event callback runs on a
 * dedicated task with an internal-RAM stack (flash/NVS access is allowed), but
 * it must not block for long: events are queued and get dropped when the queue
 * overflows.
 *
 * Prerequisites: WiFi connected (IP address assigned), nvs_flash_init() done,
 * audio_out initialised and started, esp_netif + default event loop created.
 *
 * Status: builds and links; exercised in QEMU up to the first network request
 * of the Spotify login. Everything after a successful login (playback, events,
 * metadata, credential storage) has NOT run yet - see docs/SPIKE_REPORT.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPOTIFY_BITRATE_96  = 96,
    SPOTIFY_BITRATE_160 = 160,
    SPOTIFY_BITRATE_320 = 320,   /* needs Spotify Premium */
} spotify_bitrate_t;

typedef enum {
    /** Advertising via mDNS, waiting for the app to pick the device. */
    SPOTIFY_EVENT_ZEROCONF_WAITING,
    /** Logged in to Spotify. data.session is valid. */
    SPOTIFY_EVENT_LOGGED_IN,
    /** Login was rejected (e.g. stored credentials revoked). Falls back to Zeroconf. */
    SPOTIFY_EVENT_LOGIN_FAILED,
    /** A (new) playback was started by a controller. data.playback is valid. */
    SPOTIFY_EVENT_PLAYBACK_STARTED,
    /** Playing <-> paused. data.playback is valid. */
    SPOTIFY_EVENT_PLAYBACK_PAUSED,
    SPOTIFY_EVENT_PLAYBACK_RESUMED,
    /** Seek within the current track. data.playback is valid. */
    SPOTIFY_EVENT_SEEK,
    /** The audible track changed. data.track is valid. */
    SPOTIFY_EVENT_TRACK_CHANGED,
    /** Volume set by a controller. data.volume is valid (0..65535); it is
     *  applied to the audio output (audio_out_set_volume). */
    SPOTIFY_EVENT_VOLUME,
    /** The play queue ran out; nothing left to play. cspot then re-loads the
     *  last track in paused state, so expect PLAYBACK_STARTED followed by
     *  PLAYBACK_PAUSED right after this event. */
    SPOTIFY_EVENT_PLAYBACK_STOPPED,
    /** Another device took over playback, or the session ended. */
    SPOTIFY_EVENT_INACTIVE,
    /** spotify_connect_play_uri() could not resolve or start the context. */
    SPOTIFY_EVENT_PLAY_URI_FAILED,
} spotify_event_id_t;

/** Track metadata. Strings are UTF-8, never NULL, and only valid during the callback. */
typedef struct {
    const char *track_id;     /* hex GID of the track or episode */
    const char *title;
    const char *artist;       /* first artist only */
    const char *album;
    const char *cover_url;    /* https://i.scdn.co/image/<id> (JPEG), may be "" */
    uint32_t    duration_ms;
    uint32_t    position_ms;  /* position at which the track became audible */
} spotify_track_t;

typedef struct {
    bool     paused;
    uint32_t position_ms;
} spotify_playback_t;

typedef struct {
    const char *username;     /* Spotify user name, only valid during the callback */
    bool        stored_credentials;   /* true: logged in from NVS, false: via Zeroconf */
} spotify_session_t;

typedef struct {
    spotify_event_id_t id;
    union {
        spotify_session_t  session;    /* LOGGED_IN */
        spotify_playback_t playback;   /* PLAYBACK_STARTED / _PAUSED / _RESUMED / SEEK */
        spotify_track_t    track;      /* TRACK_CHANGED */
        uint16_t           volume;     /* VOLUME */
    } data;
} spotify_event_t;

typedef void (*spotify_event_cb_t)(const spotify_event_t *event, void *user_ctx);

typedef struct {
    const char        *device_name;     /* name shown in the Spotify app */
    const char        *client_id;       /* your Spotify developer app; required */
    const char        *client_secret;   /* required */
    spotify_bitrate_t  bitrate;
    uint16_t           zeroconf_port;   /* TCP port of the Zeroconf HTTP endpoint; 0 = default 7864 */
    uint32_t           initial_volume;  /* 0..65535 (e.g. the last one, stored by the app);
                                           > 65535: full volume */
    spotify_event_cb_t event_cb;        /* may be NULL */
    void              *user_ctx;
} spotify_connect_config_t;

/** Snapshot of what the wrapper knows about the playback state. */
typedef struct {
    bool     logged_in;
    bool     active;          /* this device is the one currently selected for playback */
    bool     paused;
    uint32_t position_ms;     /* extrapolated while playing */
    uint32_t duration_ms;     /* of the current track, 0 if unknown */
} spotify_connect_status_t;

/**
 * Start Spotify Connect. Strings in config are copied. Returns immediately;
 * login and everything else happens on background tasks.
 *
 * @return ESP_ERR_INVALID_ARG if device name / client id / client secret are
 *         missing, ESP_ERR_INVALID_STATE if already started.
 */
esp_err_t spotify_connect_start(const spotify_connect_config_t *config);

/* Local transport controls (buttons, touch UI, NFC "tag removed" ...). They are
 * executed asynchronously and only have an effect while this device is the
 * active Spotify Connect device; the result is reported through events.
 * To START something that is not already loaded on the device (playlist,
 * album, NFC tag) use the Spotify Web API with this device as target. */
esp_err_t spotify_connect_play(void);
esp_err_t spotify_connect_pause(void);
esp_err_t spotify_connect_toggle(void);
esp_err_t spotify_connect_next(void);
esp_err_t spotify_connect_prev(void);

/** Tell Spotify (and thereby all controllers) the device volume, 0..65535. */
esp_err_t spotify_connect_set_volume(uint16_t volume);

/**
 * Start a context on this device without the app (NFC tag): a track, album,
 * playlist, artist (top tracks) or episode URI such as "spotify:playlist:<id>".
 * Works while logged in, also when the device is not the active player; the
 * device then becomes active and Spotify/the app follow. The track list is
 * resolved via Spotify's context-resolve service, with the Web API (developer
 * app credentials) as fallback. Asynchronous: success shows up as
 * PLAYBACK_STARTED / TRACK_CHANGED, failure as SPOTIFY_EVENT_PLAY_URI_FAILED.
 * @return ESP_ERR_INVALID_STATE if not logged in, ESP_ERR_INVALID_ARG for a
 *         malformed URI.
 */
esp_err_t spotify_connect_play_uri(const char *uri);

void spotify_connect_get_status(spotify_connect_status_t *out);

/**
 * Delete the stored reusable credentials. Takes effect at the next boot: the
 * device then waits for a Zeroconf login again.
 */
esp_err_t spotify_connect_forget_credentials(void);

#ifdef __cplusplus
}
#endif
