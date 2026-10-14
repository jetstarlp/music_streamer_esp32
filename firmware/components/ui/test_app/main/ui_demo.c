/*
 * UI demo for the Waveshare ESP32-S3-Touch-AMOLED-1.64 (docs/UI.md).
 *
 * Shows every UI state once with made-up data (about 40 s), then stays on
 * the player with three demo tracks and reacts to touch like the real
 * device would (buttons, swipes, settings). Brightness from the settings
 * slider goes to the panel.
 *
 * It also shows the intended app pattern: UI commands arrive in the LVGL
 * task and are posted to a queue; the app task does the work and calls
 * ui_model_publish() without taking the LVGL lock.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ui.h"

static const char *TAG = "ui_demo";

typedef struct {
    const char *title, *artist, *context;
    uint32_t duration_ms;
    float hue;
} track_t;

static const track_t s_tracks[] = {
    {"Abendrot", "AnnenMayKantereit", "Sommerhits 2026", 245000, 0.05f},
    {"Über sieben Brücken musst du gehn (Live in Köln, 1985)", "Karat & Peter Maffay",
     "Lieblingslieder für Sonntagmorgen", 312000, 0.58f},
    {"Señorita, ça va? – Café Olé", "Zoë Ågård & The Ölkännchen", "Sommerhits 2026", 198000, 0.85f},
};
#define N_TRACKS (sizeof(s_tracks) / sizeof(s_tracks[0]))

typedef struct {
    ui_cmd_t cmd;
    int32_t arg;
} cmd_msg_t;

static QueueHandle_t s_cmds;
static ui_model_t s_m;
static int s_track;
static int64_t s_pos_base_us; /* esp_timer time when position was s_m.position_ms */
static uint16_t *s_cover[N_TRACKS];

/* ---- fake covers: gradient + sun, one hue per track (in PSRAM) ---------- */

static uint16_t rgb565(float r, float g, float b)
{
    int R = (int)(fminf(fmaxf(r, 0), 1) * 255), G = (int)(fminf(fmaxf(g, 0), 1) * 255),
        B = (int)(fminf(fmaxf(b, 0), 1) * 255);
    return (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
}

static void hue_rgb(float h, float *r, float *g, float *b)
{
    *r = fabsf(h * 6 - 3) - 1;
    *g = 2 - fabsf(h * 6 - 2);
    *b = 2 - fabsf(h * 6 - 4);
}

static uint16_t *make_cover(float hue)
{
    const int n = UI_COVER_SIZE;
    uint16_t *px = heap_caps_malloc(n * n * 2, MALLOC_CAP_SPIRAM);
    if (!px) {
        return NULL;
    }
    float hr, hg, hb;
    hue_rgb(hue, &hr, &hg, &hb);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (float)y / n;
            float r = 0.10f + 0.6f * t * hr, g = 0.10f + 0.6f * t * hg, b = 0.20f + 0.5f * t * hb;
            float dx = x - n * 0.62f, dy = y - n * 0.55f;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < 34) {
                r = 0.98f, g = 0.80f, b = 0.50f;
            }
            else if (d < 60) {
                float k = 1 - (d - 34) / 26;
                r += 0.25f * k, g += 0.15f * k;
            }
            if (y > n * 0.7f) {
                r *= 0.35f, g *= 0.3f, b *= 0.5f;
            }
            px[y * n + x] = rgb565(r, g, b);
        }
    }
    return px;
}

/* ---- model helpers (app task only) ---------------------------------------- */

static uint32_t position_now(void)
{
    if (s_m.state != UI_STATE_PLAYING) {
        return s_m.position_ms;
    }
    int64_t ms = s_m.position_ms + (esp_timer_get_time() - s_pos_base_us) / 1000;
    return ms > s_m.duration_ms ? s_m.duration_ms : (uint32_t)ms;
}

static void set_track(int idx, bool playing)
{
    s_track = (idx + (int)N_TRACKS) % (int)N_TRACKS;
    const track_t *t = &s_tracks[s_track];
    snprintf(s_m.title, sizeof(s_m.title), "%s", t->title);
    snprintf(s_m.artist, sizeof(s_m.artist), "%s", t->artist);
    snprintf(s_m.context, sizeof(s_m.context), "%s", t->context);
    s_m.duration_ms = t->duration_ms;
    s_m.position_ms = 0;
    s_pos_base_us = esp_timer_get_time();
    s_m.state = playing ? UI_STATE_PLAYING : UI_STATE_PAUSED;
    s_m.playing = playing;
    s_m.cover_rgb565 = s_cover[s_track];
    s_m.cover_w = s_m.cover_h = UI_COVER_SIZE;
}

static void toast(ui_toast_kind_t kind, const char *text)
{
    s_m.toast_kind = kind;
    snprintf(s_m.toast, sizeof(s_m.toast), "%s", text);
    s_m.toast_id++;
}

static void publish(void)
{
    ui_model_publish(&s_m);
}

/* ---- UI commands: LVGL task -> queue -> app task -------------------------- */

static void on_ui_cmd(ui_cmd_t cmd, int32_t arg, void *ctx)
{
    (void)ctx;
    cmd_msg_t msg = {cmd, arg};
    xQueueSend(s_cmds, &msg, 0); /* never block in the LVGL task */
}

static void handle(const cmd_msg_t *c)
{
    ESP_LOGI(TAG, "command %d arg %ld", (int)c->cmd, (long)c->arg);
    switch (c->cmd) {
    case UI_CMD_TOGGLE:
        s_m.position_ms = position_now();
        s_pos_base_us = esp_timer_get_time();
        s_m.state = s_m.state == UI_STATE_PLAYING ? UI_STATE_PAUSED : UI_STATE_PLAYING;
        s_m.playing = s_m.state == UI_STATE_PLAYING;
        break;
    case UI_CMD_NEXT:
        set_track(s_track + 1, true);
        break;
    case UI_CMD_PREV:
        if (position_now() > 3000) {
            s_m.position_ms = 0;
            s_pos_base_us = esp_timer_get_time();
        }
        else {
            set_track(s_track - 1, true);
        }
        break;
    case UI_CMD_SHUFFLE:
        s_m.shuffle = c->arg != 0;
        break;
    case UI_CMD_SET_BRIGHTNESS:
        s_m.brightness = (uint8_t)c->arg;
        display_set_brightness(s_m.brightness);
        return; /* no publish needed while the slider moves */
    case UI_CMD_SET_TAG_REMOVE_ACTION:
        s_m.tag_remove_action = (uint8_t)c->arg;
        break;
    case UI_CMD_CHECK_UPDATE:
        s_m.update_checking = true;
        publish();
        vTaskDelay(pdMS_TO_TICKS(1500));
        s_m.update_checking = false;
        s_m.update_available = true;
        snprintf(s_m.update_version, sizeof(s_m.update_version), "1.1.0");
        break;
    case UI_CMD_START_UPDATE:
        toast(UI_TOAST_INFO, "Demo: kein echtes Update");
        s_m.update_available = false;
        snprintf(s_m.update_text, sizeof(s_m.update_text), "Aktuell");
        break;
    case UI_CMD_OPEN_SETUP:
        toast(UI_TOAST_INFO, "Demo: WLAN-Einrichtung nicht aktiv");
        break;
    case UI_CMD_WRITE_TAG:
        toast(UI_TOAST_NFC, "Demo: Tag beschreiben kommt per Update");
        break;
    default:
        return;
    }
    s_m.position_ms = position_now();
    s_pos_base_us = esp_timer_get_time();
    publish();
}

/* ---- tour through all states ------------------------------------------------- */

static void wait_ms(uint32_t ms)
{
    cmd_msg_t c;
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(50))) {
            handle(&c);
        }
    }
}

static void tour(void)
{
    s_m.state = UI_STATE_BOOT;
    publish();
    wait_ms(2500);

    s_m.state = UI_STATE_SETUP_WIFI;
    snprintf(s_m.setup_qr, sizeof(s_m.setup_qr), "WIFI:T:nopass;S:Music-Streamer;;");
    snprintf(s_m.setup_ssid, sizeof(s_m.setup_ssid), "Music-Streamer");
    snprintf(s_m.setup_url, sizeof(s_m.setup_url), "http://192.168.4.1");
    publish();
    wait_ms(5000);

    s_m.state = UI_STATE_CONNECTING_WIFI;
    s_m.wifi_ok = false;
    publish();
    wait_ms(4000);

    s_m.wifi_ok = true;
    s_m.state = UI_STATE_WAIT_SPOTIFY;
    publish();
    wait_ms(4000);

    s_m.state = UI_STATE_IDLE;
    publish();
    wait_ms(4000);

    toast(UI_TOAST_NFC, "Playlist „Sommerhits 2026“ wird gestartet …");
    s_m.tag_present = true;
    publish();
    wait_ms(1500);
    set_track(0, true);
    publish();
    wait_ms(6000);

    s_m.state = UI_STATE_UPDATE;
    for (int p = 0; p <= 100; p += 4) {
        s_m.update_progress = (uint8_t)p;
        snprintf(s_m.update_text, sizeof(s_m.update_text), "Demo – bitte nicht ausschalten");
        publish();
        wait_ms(120);
    }
    s_m.update_text[0] = '\0';

    s_m.state = UI_STATE_ERROR;
    snprintf(s_m.error_title, sizeof(s_m.error_title), "Spotify nicht erreichbar");
    snprintf(s_m.error_text, sizeof(s_m.error_text), "Neuer Versuch in 30 Sekunden.");
    publish();
    wait_ms(4000);

    s_m.tag_present = false;
    set_track(0, true);
    toast(UI_TOAST_INFO, "Demo: jetzt selbst ausprobieren");
    publish();
}

static void app_task(void *arg)
{
    (void)arg;
    tour();
    cmd_msg_t c;
    int64_t next_log = 0;
    for (;;) {
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(500))) {
            handle(&c);
        }
        if (s_m.state == UI_STATE_PLAYING && position_now() >= s_m.duration_ms) {
            set_track(s_track + 1, true);
            publish();
        }
        if (esp_timer_get_time() > next_log) {
            next_log = esp_timer_get_time() + 10 * 1000000LL;
            ESP_LOGI(TAG, "internal free %u (min %u), PSRAM free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

void app_main(void)
{
    for (size_t i = 0; i < N_TRACKS; i++) {
        s_cover[i] = make_cover(s_tracks[i].hue);
    }
    s_cmds = xQueueCreate(16, sizeof(cmd_msg_t));

    s_m.wifi_ok = true;
    s_m.linked = true;
    snprintf(s_m.device_name, sizeof(s_m.device_name), "Wohnzimmer");
    snprintf(s_m.wifi_ssid, sizeof(s_m.wifi_ssid), "Demo-WLAN");
    s_m.rssi = -61;
    snprintf(s_m.ip, sizeof(s_m.ip), "192.168.178.23");
    snprintf(s_m.fw_version, sizeof(s_m.fw_version), "ui-demo");
    s_m.brightness = display_get_brightness();

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    lv_display_t *disp = display_init();
    if (!disp) {
        ESP_LOGE(TAG, "display_init failed");
        return;
    }
    ui_set_command_handler(on_ui_cmd, NULL);
    ui_set_screensaver(120);
    display_lock(0);
    ui_init(disp);
    display_unlock();
    ESP_LOGI(TAG, "display + ui use %u bytes internal RAM",
             (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    xTaskCreate(app_task, "app", 4096, NULL, 4, NULL);
}
