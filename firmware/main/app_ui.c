/*
 * app_ui - see app_ui.h.
 *
 * Rules (docs/ARCHITECTURE.md section 4):
 *  - Tag with a Spotify link placed: start it on this device
 *    (spotify_connect_play_uri). The same tag placed again while its music is
 *    paused: resume instead of restarting.
 *  - Tag removed: pause / keep playing / pause at the end of the current track
 *    (setting "tag_remove").
 *  - The screen follows the most important state: update > setup hotspot >
 *    no WiFi > Spotify not ready > playing/paused > idle.
 */
#include "app_ui.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_cover.h"
#include "app_settings.h"
#include "board_pins.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "nfc_tag.h"
#include "settings.h"
#include "ui_model.h"

#include "sdkconfig.h"

/* The QEMU test build has no display hardware (the QSPI panel init would hang). */
#if BOARD_HAS_DISPLAY && !CONFIG_MS_NET_QEMU_OPENETH
#define APP_UI_DISPLAY 1
#include "display.h"
#include "ui.h"
#else
#define APP_UI_DISPLAY 0
#endif

static const char *TAG = "app_ui";

#define CTL_QUEUE_LEN     16
#define CTL_TASK_STACK    6144
#define CTL_TASK_PRIO     4
#define CTL_TICK_MS       500
#define NFC_POLL_IDLE_MS  300
#define NFC_POLL_PLAY_MS  750

typedef enum {
    MSG_UI_CMD,
    MSG_SPOTIFY,
    MSG_TRACK,
    MSG_NFC,
    MSG_PORTAL,
    MSG_OTA,
    MSG_COVER,
} msg_kind_t;

typedef struct {
    char title[96], artist[96], album[64], cover_url[128];
    uint32_t duration_ms, position_ms;
} track_msg_t;

typedef struct {
    msg_kind_t kind;
    union {
        struct { ui_cmd_t cmd; int32_t arg; } ui;
        struct { spotify_event_id_t id; bool paused; uint32_t position_ms; } sp;
        track_msg_t *track;                 /* heap, freed by the controller */
        nfc_event_t nfc;
        net_portal_event_t portal;
        struct { uint32_t id; uint16_t *rgb565; uint16_t size; } cover;
    } u;
} ctl_msg_t;

static struct {
    QueueHandle_t queue;
    ui_model_t    m;                     /* controller task only */
    bool          has_display;

    /* Spotify */
    bool          have_track;
    bool          playing;
    char          cover_url[128];
    uint32_t      cover_req;
    uint16_t     *cover;                 /* current cover, PSRAM, owned here */

    /* NFC */
    bool          tag_present;
    bool          tag_has_ref;
    spotify_ref_t tag_ref;               /* tag on the reader (or last one) */
    bool          tag_waits_for_login;   /* placed before Spotify was connected */
    uint8_t       tag_uid[7];            /* UID of the tag tag_ref came from */
    uint8_t       tag_uid_len;
    bool          tag_context_active;    /* the music playing was started by that tag */
    bool          pause_after_track;     /* "Titel zu Ende" after the tag was removed */

    /* portal */
    bool          portal_active;

    /* settings */
    bool          settings_dirty;
} s;

/* ---- event entry points (any task) ------------------------------------------------ */

static void post(const ctl_msg_t *msg)
{
    if (s.queue == NULL || xQueueSend(s.queue, msg, 0) != pdTRUE) {
        if (msg->kind == MSG_TRACK) {
            free(msg->u.track);
        } else if (msg->kind == MSG_COVER) {
            heap_caps_free(msg->u.cover.rgb565);
        }
    }
}

void app_ui_spotify_event(const spotify_event_t *ev)
{
    ctl_msg_t msg = {0};
    if (ev->id == SPOTIFY_EVENT_TRACK_CHANGED) {
        track_msg_t *t = calloc(1, sizeof(*t));
        if (t == NULL) {
            return;
        }
        strlcpy(t->title, ev->data.track.title, sizeof(t->title));
        strlcpy(t->artist, ev->data.track.artist, sizeof(t->artist));
        strlcpy(t->album, ev->data.track.album, sizeof(t->album));
        strlcpy(t->cover_url, ev->data.track.cover_url, sizeof(t->cover_url));
        t->duration_ms = ev->data.track.duration_ms;
        t->position_ms = ev->data.track.position_ms;
        msg.kind = MSG_TRACK;
        msg.u.track = t;
    } else {
        msg.kind = MSG_SPOTIFY;
        msg.u.sp.id = ev->id;
        if (ev->id == SPOTIFY_EVENT_PLAYBACK_STARTED || ev->id == SPOTIFY_EVENT_PLAYBACK_PAUSED ||
            ev->id == SPOTIFY_EVENT_PLAYBACK_RESUMED || ev->id == SPOTIFY_EVENT_SEEK) {
            msg.u.sp.paused = ev->data.playback.paused;
            msg.u.sp.position_ms = ev->data.playback.position_ms;
        }
    }
    post(&msg);
}

void app_ui_nfc_event(const nfc_event_t *ev)
{
    ctl_msg_t msg = {.kind = MSG_NFC};
    msg.u.nfc = *ev;
    post(&msg);
}

void app_ui_portal_event(const net_portal_event_t *ev)
{
    ctl_msg_t msg = {.kind = MSG_PORTAL};
    msg.u.portal = *ev;
    post(&msg);
}

void app_ui_ota_event(const net_ota_status_t *st)
{
    (void)st;   /* the controller reads the full status itself */
    ctl_msg_t msg = {.kind = MSG_OTA};
    post(&msg);
}

static void on_cover(uint32_t id, uint16_t *rgb565, uint16_t size, void *ctx)
{
    (void)ctx;
    ctl_msg_t msg = {.kind = MSG_COVER};
    msg.u.cover.id = id;
    msg.u.cover.rgb565 = rgb565;
    msg.u.cover.size = size;
    post(&msg);
}

#if APP_UI_DISPLAY
/* LVGL task, LVGL lock held: never block. */
static void on_ui_command(ui_cmd_t cmd, int32_t arg, void *ctx)
{
    (void)ctx;
    if (cmd == UI_CMD_SET_BRIGHTNESS) {
        display_set_brightness((uint8_t)arg);   /* at once, the slider is live */
    }
    ctl_msg_t msg = {.kind = MSG_UI_CMD};
    msg.u.ui.cmd = cmd;
    msg.u.ui.arg = arg;
    post(&msg);
}
#endif

/* ---- helpers ----------------------------------------------------------------------- */

static void toast(ui_toast_kind_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void toast(ui_toast_kind_t kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.m.toast, sizeof(s.m.toast), fmt, ap);
    va_end(ap);
    s.m.toast_kind = kind;
    s.m.toast_id++;
    ESP_LOGI(TAG, "toast: %s", s.m.toast);
}

static const char *ref_kind_de(spotify_ref_type_t t)
{
    switch (t) {
    case SPOTIFY_REF_TRACK:    return "Titel";
    case SPOTIFY_REF_ALBUM:    return "Album";
    case SPOTIFY_REF_PLAYLIST: return "Playlist";
    case SPOTIFY_REF_ARTIST:   return "Künstler";
    case SPOTIFY_REF_SHOW:     return "Podcast";
    case SPOTIFY_REF_EPISODE:  return "Folge";
    }
    return "Musik";
}

static void save_settings(void)
{
    if (!s.settings_dirty) {
        return;
    }
    settings_set_u32(SETTINGS_KEY_BRIGHTNESS, s.m.brightness);
    settings_set_u32(SETTINGS_KEY_TAG_REMOVE_ACTION, s.m.tag_remove_action);
    settings_set_u32(SETTINGS_KEY_SAVER_MINUTES, s.m.saver_minutes);
    s.settings_dirty = false;
}

static void refresh_network(void)
{
    net_wifi_info_t wi;
    net_wifi_get_info(&wi);
    s.m.wifi_ok = app_network_up();
    strlcpy(s.m.wifi_ssid, wi.ssid, sizeof(s.m.wifi_ssid));
    s.m.rssi = wi.state == NET_WIFI_CONNECTED ? wi.rssi : 0;
    uint32_t ip = app_network_ip();
    if (ip) {
        esp_ip4_addr_t a = {.addr = ip};
        snprintf(s.m.ip, sizeof(s.m.ip), IPSTR, IP2STR(&a));
    } else {
        s.m.ip[0] = '\0';
    }
}

static void refresh_ota(void)
{
    net_ota_status_t st;
    net_ota_get_status(&st);
    s.m.update_available = st.state == NET_OTA_AVAILABLE;
    s.m.update_checking = st.state == NET_OTA_CHECKING;
    strlcpy(s.m.update_version, st.available_version, sizeof(s.m.update_version));
    s.m.update_progress = st.state == NET_OTA_DOWNLOADING ? st.progress
                        : (st.state == NET_OTA_READY || st.state == NET_OTA_REBOOTING) ? 101 : 0;
    switch (st.state) {
    case NET_OTA_DISABLED:    strlcpy(s.m.update_text, "Updates nicht eingerichtet", sizeof(s.m.update_text)); break;
    case NET_OTA_CHECKING:    strlcpy(s.m.update_text, "Suche nach Updates …", sizeof(s.m.update_text)); break;
    case NET_OTA_UP_TO_DATE:  strlcpy(s.m.update_text, "Software ist aktuell", sizeof(s.m.update_text)); break;
    case NET_OTA_AVAILABLE:   snprintf(s.m.update_text, sizeof(s.m.update_text), "Neu: Version %s", st.available_version); break;
    case NET_OTA_DOWNLOADING: strlcpy(s.m.update_text, "Wird geladen – bitte nicht ausschalten",
                                      sizeof(s.m.update_text)); break;
    case NET_OTA_VERIFYING:   strlcpy(s.m.update_text, "Wird geprüft …", sizeof(s.m.update_text)); break;
    case NET_OTA_READY:
    case NET_OTA_REBOOTING:   strlcpy(s.m.update_text, "Neustart …", sizeof(s.m.update_text)); break;
    case NET_OTA_FAILED:      strlcpy(s.m.update_text, "Update fehlgeschlagen", sizeof(s.m.update_text)); break;
    default:                  s.m.update_text[0] = '\0'; break;
    }
}

/* Which screen: the most important state wins. */
static void compute_state(void)
{
    net_ota_status_t ota;
    net_ota_get_status(&ota);
    app_spotify_info_t sp;
    app_get_spotify_info(&sp);
    spotify_connect_status_t st;
    spotify_connect_get_status(&st);

    if (ota.state == NET_OTA_DOWNLOADING || ota.state == NET_OTA_VERIFYING ||
        ota.state == NET_OTA_READY || ota.state == NET_OTA_REBOOTING) {
        s.m.state = UI_STATE_UPDATE;
    } else if (s.portal_active) {
        s.m.state = UI_STATE_SETUP_WIFI;
    } else if (!app_network_up()) {
        s.m.state = UI_STATE_CONNECTING_WIFI;
    } else if (sp.state == APP_SPOTIFY_NOT_CONFIGURED) {
        s.m.state = UI_STATE_ERROR;
        strlcpy(s.m.error_title, "Spotify fehlt", sizeof(s.m.error_title));
        strlcpy(s.m.error_text, "Die Zugangsdaten für Spotify sind noch nicht eingetragen.",
                sizeof(s.m.error_text));
    } else if (sp.state == APP_SPOTIFY_START_FAILED) {
        s.m.state = UI_STATE_ERROR;
        strlcpy(s.m.error_title, "Spotify nicht erreichbar", sizeof(s.m.error_title));
        strlcpy(s.m.error_text, "Neuer Versuch läuft. Prüfe sonst, ob das WLAN funktioniert.",
                sizeof(s.m.error_text));
    } else if (sp.state == APP_SPOTIFY_WAITING_LOGIN) {
        s.m.state = UI_STATE_WAIT_SPOTIFY;
    } else if (sp.state != APP_SPOTIFY_LOGGED_IN) {
        s.m.state = UI_STATE_CONNECTING_WIFI;     /* "Verbinde …" while Spotify logs in */
    } else if (st.active && s.have_track) {
        s.m.state = st.paused ? UI_STATE_PAUSED : UI_STATE_PLAYING;
    } else {
        s.m.state = UI_STATE_IDLE;
    }

    s.playing = st.active && !st.paused && s.have_track;
    s.m.playing = s.m.state == UI_STATE_PLAYING;
    if (st.active && s.have_track) {
        s.m.position_ms = st.position_ms;
        if (st.duration_ms) {
            s.m.duration_ms = st.duration_ms;
        }
    }
}

static void publish(void)
{
    compute_state();
    s.m.tag_present = s.tag_present;
#if APP_UI_DISPLAY
    if (s.has_display) {
        ui_model_publish(&s.m);
    }
#endif
}

/* ---- rules ----------------------------------------------------------------------------- */

static void on_tag_placed(const nfc_event_t *ev)
{
    s.tag_present = true;
    bool same_uid = s.tag_has_ref && ev->uid_len > 0 && ev->uid_len == s.tag_uid_len &&
                    memcmp(ev->uid, s.tag_uid, ev->uid_len) == 0;
    spotify_ref_t ref;
    if (ev->has_ref) {
        ref = ev->ref;
    } else if (!ev->read_ok && same_uid) {
        /* the tag we know, only its memory could not be read this time
         * (weak coupling): use what it said before */
        ref = s.tag_ref;
    } else {
        s.tag_has_ref = false;
        toast(UI_TOAST_WARN, "%s", ev->read_ok ? "Tag ohne Spotify-Link" : "Tag nicht lesbar – bitte neu auflegen");
        return;
    }
    char uri[SPOTIFY_REF_URI_MAX];
    spotify_ref_to_uri(&ref, uri, sizeof(uri));
    /* spotify_ref_equal: memcmp would also compare the padding byte */
    bool same = s.tag_has_ref && spotify_ref_equal(&s.tag_ref, &ref);
    s.tag_has_ref = true;
    s.tag_ref = ref;
    s.tag_uid_len = ev->uid_len <= sizeof(s.tag_uid) ? ev->uid_len : sizeof(s.tag_uid);
    memcpy(s.tag_uid, ev->uid, s.tag_uid_len);
    s.pause_after_track = false;

    spotify_connect_status_t st;
    spotify_connect_get_status(&st);
    if (!st.logged_in) {
        s.tag_waits_for_login = true;   /* started on SPOTIFY_EVENT_LOGGED_IN */
        toast(UI_TOAST_INFO, "Startet, sobald Spotify verbunden ist");
        return;
    }
    if (same && s.tag_context_active && st.active) {
        /* the same tag again: just continue */
        if (st.paused) {
            spotify_connect_play();
            toast(UI_TOAST_NFC, "Weiter geht's");
        }
        return;
    }
    esp_err_t err = spotify_connect_play_uri(uri);
    if (err == ESP_OK) {
        s.tag_context_active = true;
        toast(UI_TOAST_NFC, "%s wird gestartet …", ref_kind_de(ev->ref.type));
        ESP_LOGI(TAG, "tag: starting %s", uri);
    } else {
        toast(UI_TOAST_ERROR, "Konnte die Musik nicht starten");
        ESP_LOGW(TAG, "tag: spotify_connect_play_uri(%s): %s", uri, esp_err_to_name(err));
    }
}

static void on_tag_removed(void)
{
    s.tag_present = false;
    s.tag_waits_for_login = false;
    if (!s.tag_has_ref || !s.tag_context_active) {
        return;
    }
    spotify_connect_status_t st;
    spotify_connect_get_status(&st);
    if (!st.active || st.paused) {
        return;
    }
    switch ((ui_tag_remove_action_t)s.m.tag_remove_action) {
    case UI_TAG_REMOVE_PAUSE:
        spotify_connect_pause();
        break;
    case UI_TAG_REMOVE_CONTINUE:
        break;
    case UI_TAG_REMOVE_STOP_AFTER_TRACK:
        s.pause_after_track = true;
        toast(UI_TOAST_INFO, "Pause nach diesem Titel");
        break;
    }
}

/* ---- message handling ------------------------------------------------------------- */

static void handle_ui_cmd(ui_cmd_t cmd, int32_t arg)
{
    switch (cmd) {
    case UI_CMD_TOGGLE:
        if (spotify_connect_toggle() != ESP_OK && s.tag_has_ref && s.tag_present) {
            /* nothing active on this device, but a tag lies on it: start it */
            char uri[SPOTIFY_REF_URI_MAX];
            spotify_ref_to_uri(&s.tag_ref, uri, sizeof(uri));
            spotify_connect_play_uri(uri);
        }
        break;
    case UI_CMD_NEXT:
        spotify_connect_next();
        break;
    case UI_CMD_PREV:
        spotify_connect_prev();
        break;
    case UI_CMD_SHUFFLE:
        toast(UI_TOAST_INFO, "Zufall kommt mit einem Update");
        break;
    case UI_CMD_WRITE_TAG:
        break;
    case UI_CMD_OPEN_SETUP:
        if (s.playing) {
            spotify_connect_pause();
        }
        net_portal_start();
        break;
    case UI_CMD_CANCEL:
        net_portal_stop();
        break;
    case UI_CMD_OPEN_SETTINGS:
        refresh_network();
        refresh_ota();
        break;
    case UI_CMD_CLOSE_SETTINGS:
        save_settings();
        break;
    case UI_CMD_SET_BRIGHTNESS:
        s.m.brightness = (uint8_t)(arg < 5 ? 5 : arg > 100 ? 100 : arg);
        s.settings_dirty = true;
        break;
    case UI_CMD_SET_TAG_REMOVE_ACTION:
        if (arg >= 0 && arg <= 2) {
            s.m.tag_remove_action = (uint8_t)arg;
            s.settings_dirty = true;
            save_settings();
        }
        break;
    case UI_CMD_SET_SAVER_MINUTES:
        if (arg == 1 || arg == 5 || arg == 15) {
            s.m.saver_minutes = (uint8_t)arg;
            s.settings_dirty = true;
            save_settings();
        }
        break;
    case UI_CMD_CHECK_UPDATE:
        if (net_ota_check_now() != ESP_OK) {
            toast(UI_TOAST_WARN, "Update-Suche gerade nicht möglich");
        }
        break;
    case UI_CMD_START_UPDATE:
        if (net_ota_install() != ESP_OK) {
            toast(UI_TOAST_WARN, "Update gerade nicht möglich");
        }
        break;
    }
}

static void handle_spotify(spotify_event_id_t id)
{
    switch (id) {
    case SPOTIFY_EVENT_INACTIVE:
        /* another device took over: the tag no longer "owns" the music */
        s.tag_context_active = false;
        s.pause_after_track = false;
        break;
    case SPOTIFY_EVENT_PLAYBACK_STARTED:
        break;
    case SPOTIFY_EVENT_PLAY_URI_FAILED:
        s.tag_context_active = false;
        toast(UI_TOAST_ERROR, "Musik vom Tag lässt sich nicht starten");
        break;
    case SPOTIFY_EVENT_LOGGED_IN:
        if (s.tag_waits_for_login && s.tag_present && s.tag_has_ref) {
            char uri[SPOTIFY_REF_URI_MAX];
            spotify_ref_to_uri(&s.tag_ref, uri, sizeof(uri));
            esp_err_t err = spotify_connect_play_uri(uri);
            if (err == ESP_OK) {
                s.tag_context_active = true;
                toast(UI_TOAST_NFC, "%s wird gestartet …", ref_kind_de(s.tag_ref.type));
                ESP_LOGI(TAG, "tag: starting %s (placed before login)", uri);
            } else {
                ESP_LOGW(TAG, "tag: could not start %s after login: %s", uri, esp_err_to_name(err));
            }
        }
        s.tag_waits_for_login = false;
        break;
    case SPOTIFY_EVENT_LOGIN_FAILED:
        toast(UI_TOAST_WARN, "Spotify-Anmeldung abgelaufen – Gerät in der App neu wählen");
        break;
    default:
        break;
    }
}

static void handle_track(track_msg_t *t)
{
    strlcpy(s.m.title, t->title, sizeof(s.m.title));
    strlcpy(s.m.artist, t->artist, sizeof(s.m.artist));
    strlcpy(s.m.context, t->album, sizeof(s.m.context));
    s.m.duration_ms = t->duration_ms;
    s.m.position_ms = t->position_ms;
    if (!s.have_track) {
        s.have_track = true;
    } else if (s.pause_after_track) {
        /* "Titel zu Ende": the next track just began */
        s.pause_after_track = false;
        spotify_connect_pause();
    }
    if (strcmp(t->cover_url, s.cover_url) != 0) {
        strlcpy(s.cover_url, t->cover_url, sizeof(s.cover_url));
        s.cover_req++;
        if (s.cover_url[0]) {
            app_cover_request(s.cover_url, s.cover_req);
        } else {
            heap_caps_free(s.cover);
            s.cover = NULL;
            s.m.cover_rgb565 = NULL;
            s.m.cover_id++;
        }
    }
}

static void handle_cover(uint32_t id, uint16_t *img, uint16_t size)
{
    if (id != s.cover_req) {
        heap_caps_free(img);     /* outdated */
        return;
    }
    heap_caps_free(s.cover);
    s.cover = img;               /* NULL = placeholder */
    s.m.cover_rgb565 = img;
    s.m.cover_w = s.m.cover_h = size;
    s.m.cover_id++;
}

static void handle_portal(const net_portal_event_t *ev)
{
    switch (ev->id) {
    case NET_PORTAL_EVT_STARTED:
        s.portal_active = true;
        net_portal_get_qr_payload(s.m.setup_qr, sizeof(s.m.setup_qr));
        net_portal_get_ssid(s.m.setup_ssid, sizeof(s.m.setup_ssid));
        strlcpy(s.m.setup_url, net_portal_get_url(), sizeof(s.m.setup_url));
        s.m.setup_cancelable = ev->requested && app_network_up();
        break;
    case NET_PORTAL_EVT_STOPPED:
        s.portal_active = false;
        break;
    case NET_PORTAL_EVT_CREDENTIALS:
        toast(UI_TOAST_INFO, "Verbinde mit „%s“ …", ev->ssid);
        break;
    case NET_PORTAL_EVT_CONNECT_OK:
        toast(UI_TOAST_INFO, "Mit „%s“ verbunden", ev->ssid);
        break;
    case NET_PORTAL_EVT_CONNECT_FAILED:
        toast(UI_TOAST_WARN, "„%s“: %s", ev->ssid, net_wifi_reason_text_de(ev->reason));
        break;
    default:
        break;
    }
}

static void ctl_task(void *arg)
{
    (void)arg;
    ctl_msg_t msg;
    int64_t last_publish = 0;
    for (;;) {
        bool changed = false;
        if (xQueueReceive(s.queue, &msg, pdMS_TO_TICKS(CTL_TICK_MS)) == pdTRUE) {
            changed = true;
            switch (msg.kind) {
            case MSG_UI_CMD:
                handle_ui_cmd(msg.u.ui.cmd, msg.u.ui.arg);
                break;
            case MSG_SPOTIFY:
                handle_spotify(msg.u.sp.id);
                break;
            case MSG_TRACK:
                handle_track(msg.u.track);
                free(msg.u.track);
                break;
            case MSG_NFC:
                switch (msg.u.nfc.id) {
                case NFC_EVT_TAG_PRESENT:
                    on_tag_placed(&msg.u.nfc);
                    break;
                case NFC_EVT_TAG_WRITTEN:
                    s.tag_present = true;
                    toast(UI_TOAST_NFC, "Tag beschrieben");
                    break;
                case NFC_EVT_TAG_REMOVED:
                    on_tag_removed();
                    break;
                default:
                    break;
                }
                break;
            case MSG_PORTAL:
                handle_portal(&msg.u.portal);
                break;
            case MSG_OTA:
                refresh_ota();
                break;
            case MSG_COVER:
                handle_cover(msg.u.cover.id, msg.u.cover.rgb565, msg.u.cover.size);
                break;
            }
        }
        int64_t now = esp_timer_get_time();
        if (changed || now - last_publish > 1000000) {
            refresh_network();
            nfc_set_poll_interval_ms(s.playing ? NFC_POLL_PLAY_MS : NFC_POLL_IDLE_MS);
            if (s.m.state == UI_STATE_UPDATE) {
                refresh_ota();
            }
            publish();
            last_publish = now;
        }
        /* brightness changes come in bursts from the slider: save when quiet */
        if (s.settings_dirty && !changed) {
            save_settings();
        }
    }
}

void app_ui_start(void)
{
    s.m.brightness = (uint8_t)settings_get_u32_or(SETTINGS_KEY_BRIGHTNESS, 70);
    if (s.m.brightness < 5 || s.m.brightness > 100) {
        s.m.brightness = 70;
    }
    s.m.tag_remove_action = (uint8_t)settings_get_u32_or(SETTINGS_KEY_TAG_REMOVE_ACTION, UI_TAG_REMOVE_PAUSE);
    if (s.m.tag_remove_action > 2) {
        s.m.tag_remove_action = UI_TAG_REMOVE_PAUSE;
    }
    s.m.saver_minutes = (uint8_t)settings_get_u32_or(SETTINGS_KEY_SAVER_MINUTES, 5);
    if (s.m.saver_minutes != 1 && s.m.saver_minutes != 5 && s.m.saver_minutes != 15) {
        s.m.saver_minutes = 5;
    }
    app_settings_device_name(s.m.device_name, sizeof(s.m.device_name));
    strlcpy(s.m.fw_version, esp_app_get_description()->version, sizeof(s.m.fw_version));
    s.m.tag_write_supported = false;
    s.m.state = UI_STATE_BOOT;

    s.queue = xQueueCreate(CTL_QUEUE_LEN, sizeof(ctl_msg_t));
    if (s.queue == NULL) {
        ESP_LOGE(TAG, "no memory");
        return;
    }

#if APP_UI_DISPLAY
    lv_display_t *disp = display_init();
    if (disp) {
        display_set_brightness(s.m.brightness);
        if (display_lock(0)) {
            ui_init(disp);
            ui_set_command_handler(on_ui_command, NULL);
            display_unlock();
            s.has_display = true;
            ui_model_publish(&s.m);
        }
    } else {
        ESP_LOGE(TAG, "display not available");
    }
#endif
    if (app_cover_start(on_cover, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "cover loader not available");
    }
    if (xTaskCreatePinnedToCore(ctl_task, "app_ctl", CTL_TASK_STACK, NULL, CTL_TASK_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "app_ctl task not started");
    }
}
