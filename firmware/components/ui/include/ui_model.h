/*
 * ui_model.h - the only connection between the UI and the rest of the
 * firmware (docs/ARCHITECTURE.md section 3). Everything below "additions"
 * extends the binding interface; nothing of it was renamed.
 *
 * The app owns a ui_model_t, fills it and hands a copy to the UI with
 * ui_model_publish() whenever something changes. The UI draws only what is
 * in the model; it never asks the app for anything.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The UI shows covers at UI_COVER_SIZE x UI_COVER_SIZE; other sizes are scaled. */
#define UI_COVER_SIZE 180

/** Which full screen is shown. Zero-initialised model = BOOT. */
typedef enum {
    UI_STATE_BOOT = 0,       /**< splash "Music Streamer – startet …" */
    UI_STATE_SETUP_WIFI,     /**< QR code (setup_qr) + hotspot name (setup_ssid) + address (setup_url) */
    UI_STATE_CONNECTING_WIFI,/**< "Verbinde mit »wifi_ssid« …" */
    UI_STATE_WAIT_SPOTIFY,   /**< "Öffne Spotify, wähle »device_name«" (first start, nothing played yet) */
    UI_STATE_PLAYING,        /**< now playing (title, artist, cover, progress) */
    UI_STATE_PAUSED,         /**< now playing, paused look */
    UI_STATE_IDLE,           /**< nothing playing: clock, "Tag auflegen" */
    UI_STATE_UPDATE,         /**< firmware update: update_progress %, update_text */
    UI_STATE_ERROR,          /**< error_title + error_text, settings stay reachable */
    UI_STATE_SETUP_SPOTIFY,  /**< QR code (setup_qr) for the Spotify login, setup_url as text */
} ui_state_t;

/** What happens when the tag is taken off the reader (settings). */
typedef enum {
    UI_TAG_REMOVE_PAUSE = 0,           /**< "Pausieren" */
    UI_TAG_REMOVE_CONTINUE = 1,        /**< "Weiterspielen" */
    UI_TAG_REMOVE_STOP_AFTER_TRACK = 2,/**< "Titel zu Ende" */
} ui_tag_remove_action_t;

/** Look of the toast banner at the top. */
typedef enum {
    UI_TOAST_INFO = 0, /**< brass dot */
    UI_TOAST_NFC,      /**< contactless symbol: tag feedback */
    UI_TOAST_WARN,     /**< amber dot, shown longer */
    UI_TOAST_ERROR,    /**< red dot, shown longer */
} ui_toast_kind_t;

typedef struct {
    /* --- ARCHITECTURE section 3 --- */
    char title[96];   /**< UTF-8 */
    char artist[96];
    char context[64]; /**< playlist / album / artist name, may be empty */
    uint32_t duration_ms, position_ms; /**< position at the moment of publishing; the UI extrapolates */
    bool playing;     /**< must match state == UI_STATE_PLAYING (the UI uses state) */
    bool tag_present; /**< small contactless symbol on the player */
    bool wifi_ok;     /**< false: crossed-out Wi-Fi symbol on the player */
    bool linked;      /**< Spotify Web API linked (settings shows it) */
    /**
     * Cover as native-endian RGB565 (lv_color16_t, NOT byte-swapped), any
     * size, best UI_COVER_SIZE square. NULL = record placeholder. The UI
     * copies the pixels inside ui_model_publish() whenever pointer, size or
     * cover_id changed, so the buffer may be freed or reused afterwards.
     */
    const uint16_t *cover_rgb565;
    uint16_t cover_w, cover_h;

    /* --- additions --- */
    ui_state_t state;
    uint32_t cover_id;    /**< bump when the same buffer holds a new cover */
    bool shuffle;         /**< current shuffle state (settings switch) */

    char device_name[48]; /**< Spotify Connect name, e.g. "Wohnzimmer" */
    char wifi_ssid[33];   /**< connected (or connecting) network */
    int8_t rssi;          /**< dBm, 0 = unknown */
    char ip[16];          /**< shown in settings, may be empty */

    /* setup screens (SETUP_WIFI, SETUP_SPOTIFY) */
    char setup_qr[160];   /**< QR payload, e.g. "WIFI:T:nopass;S:Music-Streamer;;" or the login URL */
    char setup_ssid[33];  /**< hotspot name shown as text (SETUP_WIFI) */
    char setup_url[64];   /**< address shown as text, e.g. "http://192.168.4.1" */
    bool setup_cancelable;/**< show "Abbrechen" (sends UI_CMD_CANCEL) */

    /* toast: shown when toast_id changes and toast is not empty (4 s / 7 s, tap hides) */
    char toast[96];       /**< e.g. "Playlist wird gestartet …", "Tag ohne Spotify-Link" */
    uint32_t toast_id;
    ui_toast_kind_t toast_kind;

    /* firmware update */
    bool update_available;  /**< settings offers "Installieren" (UI_CMD_START_UPDATE) */
    bool update_checking;   /**< settings shows "Suche …" */
    char update_version[24];/**< version offered, e.g. "1.1.0" */
    uint8_t update_progress;/**< 0..100 in UI_STATE_UPDATE, > 100 = "Neustart …" */
    char update_text[64];   /**< status line, e.g. "Kein Update verfügbar" / "Lade herunter …" */

    /* UI_STATE_ERROR */
    char error_title[48];   /**< e.g. "Keine Verbindung" */
    char error_text[128];   /**< e.g. "Spotify ist gerade nicht erreichbar. Neuer Versuch in 30 s." */

    /* settings values (the app stores them, the UI only shows them) */
    uint8_t brightness;     /**< 5..100 % */
    uint8_t tag_remove_action; /**< ui_tag_remove_action_t */
    uint8_t saver_minutes;  /**< screensaver after 1/5/15 min without touch; 0 = keep ui_set_screensaver() */
    char fw_version[24];    /**< e.g. "1.0.0 (a3e6f87)" */
    bool tag_write_supported; /**< false: "Tag beschreiben" only says it comes with an update */
} ui_model_t;

/**
 * app -> ui. Thread-safe: copies the model (and a changed cover) under the
 * UI's own mutex; the LVGL task applies it within ~30 ms. Never blocks on
 * LVGL. May be called before ui_init(): the model is kept and applied once
 * the UI exists.
 */
void ui_model_publish(const ui_model_t *m);

typedef enum {
    UI_CMD_TOGGLE,          /**< play/pause (play button or tap on the cover) */
    UI_CMD_NEXT,            /**< next button or swipe left */
    UI_CMD_PREV,            /**< previous button or swipe right */
    UI_CMD_SHUFFLE,         /**< arg: 1 = shuffle on, 0 = off (settings switch) */
    UI_CMD_WRITE_TAG,       /**< settings "Tag beschreiben" (only if tag_write_supported) */
    UI_CMD_OPEN_SETUP,      /**< settings "WLAN ändern": start the setup hotspot */
    /* --- additions --- */
    UI_CMD_OPEN_WIFI_SETUP = UI_CMD_OPEN_SETUP, /**< alias */
    UI_CMD_OPEN_SETTINGS,   /**< settings opened (refresh rssi, ip, ... if wanted) */
    UI_CMD_CLOSE_SETTINGS,  /**< settings closed (good moment to save to NVS) */
    UI_CMD_SET_BRIGHTNESS,  /**< arg: 5..100 %, sent while the slider moves: apply at once, save debounced */
    UI_CMD_SET_TAG_REMOVE_ACTION, /**< arg: ui_tag_remove_action_t */
    UI_CMD_CHECK_UPDATE,    /**< settings "Prüfen" */
    UI_CMD_START_UPDATE,    /**< settings "Installieren" */
    UI_CMD_CANCEL,          /**< "Abbrechen" on a setup screen (setup_cancelable) */
    UI_CMD_SET_SAVER_MINUTES, /**< arg: 1, 5 or 15 (settings "Bildschirmschoner") */
} ui_cmd_t;

/**
 * ui -> app. The callback runs in the LVGL task with the LVGL lock held:
 * it must not block (post to a queue and return). Calling
 * ui_model_publish() from inside it is allowed. Replaces the two-argument
 * callback of ARCHITECTURE section 3 (arg added for brightness etc.).
 */
void ui_set_command_handler(void (*cb)(ui_cmd_t cmd, int32_t arg, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
