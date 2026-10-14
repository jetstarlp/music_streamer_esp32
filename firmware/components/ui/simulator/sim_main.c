/*
 * Headless simulator for the Music Streamer UI.
 *
 *   ui_sim <out_dir>
 *
 * Renders every screen/state with sample data into <out_dir>/<nn>_<name>.png
 * plus <out_dir>/overview.png (all screens tiled, captioned), then runs touch
 * tests (taps on every button, swipes, long press, settings controls,
 * scrolling, screensaver) that check the command callback, and a concurrency
 * test (publishing from a second thread while LVGL renders). Exit code 0 =
 * all tests passed.
 *
 * Time is simulated: lv_tick follows a counter advanced by run_ms(), so the
 * output is deterministic except for the wall clock shown on idle screens.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_internal.h"
#include "sim_mem.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#include "stb_image_write.h"

#define W UI_W
#define H UI_H
#define CAP_H 26

static uint16_t s_fb[W * H];
static uint16_t s_cap_fb[W * CAP_H];
static uint32_t s_ms;
static lv_display_t *s_disp, *s_cap_disp;
static lv_obj_t *s_cap_label;
static const char *s_out = "shots";

/* ---- tick, display, touch ---------------------------------------------- */

static uint32_t tick_cb(void)
{
    return s_ms;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static struct {
    bool pressed;
    int32_t x, y;
} s_touch;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) {
        s_ms += 5;
        lv_timer_handler();
    }
}

/* ---- PNG output ---------------------------------------------------------- */

typedef struct {
    char name[48];
    char caption[96];
    uint8_t *rgb; /* W x H x 3 */
} shot_t;

static shot_t s_shots[40];
static int s_nshots;

static void to_rgb(const uint16_t *src, uint8_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t c = src[i];
        uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        dst[3 * i + 0] = (uint8_t)((r * 527 + 23) >> 6);
        dst[3 * i + 1] = (uint8_t)((g * 259 + 33) >> 6);
        dst[3 * i + 2] = (uint8_t)((b * 527 + 23) >> 6);
    }
}

static void snap(const char *name, const char *caption)
{
    lv_refr_now(s_disp);
    shot_t *s = &s_shots[s_nshots++];
    snprintf(s->name, sizeof(s->name), "%02d_%s", s_nshots, name);
    snprintf(s->caption, sizeof(s->caption), "%s", caption);
    s->rgb = malloc(W * H * 3);
    to_rgb(s_fb, s->rgb, W * H);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.png", s_out, s->name);
    if (!stbi_write_png(path, W, H, 3, s->rgb, W * 3)) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    printf("  %s.png  %s\n", s->name, caption);
}

static void write_overview(void)
{
    const int cols = 3, gap = 16, cw = W + gap, ch = H + CAP_H + gap;
    int rows = (s_nshots + cols - 1) / cols;
    int ow = cols * cw + gap, oh = rows * ch + gap;
    uint8_t *img = calloc((size_t)ow * oh, 3);
    uint8_t *cap = malloc(W * CAP_H * 3);
    for (int i = 0; i < ow * oh; i++) {
        img[3 * i] = 0x1C, img[3 * i + 1] = 0x1A, img[3 * i + 2] = 0x17;
    }
    for (int k = 0; k < s_nshots; k++) {
        int x0 = gap + (k % cols) * cw, y0 = gap + (k / cols) * ch;
        for (int y = 0; y < H; y++) {
            memcpy(img + ((size_t)(y0 + y) * ow + x0) * 3, s_shots[k].rgb + (size_t)y * W * 3, W * 3);
        }
        char text[128];
        snprintf(text, sizeof(text), "%s", s_shots[k].caption);
        lv_label_set_text(s_cap_label, text);
        lv_refr_now(s_cap_disp);
        to_rgb(s_cap_fb, cap, W * CAP_H);
        for (int y = 0; y < CAP_H; y++) {
            memcpy(img + ((size_t)(y0 + H + y) * ow + x0) * 3, cap + (size_t)y * W * 3, W * 3);
        }
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/overview.png", s_out);
    stbi_write_png(path, ow, oh, 3, img, ow * 3);
    printf("  overview.png (%d x %d)\n", ow, oh);
    free(img);
    free(cap);
}

/* ---- sample data ----------------------------------------------------------- */

static uint16_t s_cover[UI_COVER_SIZE * UI_COVER_SIZE];

static uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* an "album cover": dusk gradient, low sun, horizon lines */
static void make_cover(void)
{
    const int n = UI_COVER_SIZE;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (float)y / n;
            int r = (int)(28 + 200 * t), g = (int)(40 + 70 * t), b = (int)(90 - 40 * t);
            float dx = x - n * 0.62f, dy = y - n * 0.58f;
            float d = dx * dx + dy * dy;
            if (d < 34 * 34) {
                r = 250, g = 196, b = 120;
            }
            else if (d < 60 * 60) {
                float k = 1.0f - (float)(sqrt(d) - 34) / 26;
                r += (int)(60 * k), g += (int)(40 * k);
            }
            if (y > n * 0.70f) {
                int band = (y - (int)(n * 0.70f)) / 6;
                r = (int)(r * 0.35f) + (band % 2 ? 10 : 0);
                g = (int)(g * 0.30f);
                b = (int)(b * 0.45f) + 15;
            }
            s_cover[y * n + x] = rgb565(r, g, b);
        }
    }
}

/* everything the app knows in normal operation */
static ui_model_t model_base(void)
{
    ui_model_t m = {0};
    m.wifi_ok = true;
    m.linked = true;
    snprintf(m.device_name, sizeof(m.device_name), "Wohnzimmer");
    snprintf(m.wifi_ssid, sizeof(m.wifi_ssid), "FRITZ!Box 7590 KL");
    m.rssi = -58;
    snprintf(m.ip, sizeof(m.ip), "192.168.178.23");
    snprintf(m.fw_version, sizeof(m.fw_version), "1.0.0 (a3e6f87)");
    m.brightness = 80;
    m.tag_remove_action = UI_TAG_REMOVE_PAUSE;
    return m;
}

static ui_model_t model_playing(void)
{
    ui_model_t m = model_base();
    m.state = UI_STATE_PLAYING;
    snprintf(m.title, sizeof(m.title), "Abendrot");
    snprintf(m.artist, sizeof(m.artist), "AnnenMayKantereit");
    snprintf(m.context, sizeof(m.context), "Sommerhits 2026");
    m.duration_ms = 245000;
    m.position_ms = 83000;
    m.playing = true;
    m.cover_rgb565 = s_cover;
    m.cover_w = m.cover_h = UI_COVER_SIZE;
    m.cover_id = 1;
    return m;
}

/* ---- command capture -------------------------------------------------------- */

typedef struct {
    ui_cmd_t cmd;
    int32_t arg;
} cmd_rec_t;

static cmd_rec_t s_cmds[256];
static int s_ncmds;

static void cmd_cb(ui_cmd_t c, int32_t arg, void *ctx)
{
    (void)ctx;
    if (s_ncmds < 256) {
        s_cmds[s_ncmds].cmd = c;
        s_cmds[s_ncmds].arg = arg;
        s_ncmds++;
    }
}

/* ---- touch helpers ------------------------------------------------------- */

static void touch_down(int32_t x, int32_t y)
{
    s_touch.x = x, s_touch.y = y, s_touch.pressed = true;
    run_ms(40);
}

static void touch_move(int32_t x, int32_t y, uint32_t ms)
{
    int32_t x0 = s_touch.x, y0 = s_touch.y;
    const int steps = 10;
    for (int i = 1; i <= steps; i++) {
        s_touch.x = x0 + (x - x0) * i / steps;
        s_touch.y = y0 + (y - y0) * i / steps;
        run_ms(ms / steps);
    }
}

static void touch_up(void)
{
    s_touch.pressed = false;
    run_ms(60);
}

static void tap(int32_t x, int32_t y)
{
    touch_down(x, y);
    run_ms(60);
    touch_up();
    run_ms(400); /* let a screen transition finish */
}

/* tap the centre of an object as it is on screen now */
static void tap_obj(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}

static void scroll_into_view(lv_obj_t *o)
{
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    run_ms(50);
}

static int s_fail;

static void expect(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) {
        s_fail++;
    }
}

static void clear_cmds(void)
{
    s_ncmds = 0;
}

/* exactly these commands, in this order (args checked when >= 0) */
static bool got1(ui_cmd_t c, int32_t arg)
{
    return s_ncmds == 1 && s_cmds[0].cmd == c && (arg < 0 || s_cmds[0].arg == arg);
}

static bool got2(ui_cmd_t c1, ui_cmd_t c2)
{
    return s_ncmds == 2 && s_cmds[0].cmd == c1 && s_cmds[1].cmd == c2;
}

static bool any(ui_cmd_t c)
{
    for (int i = 0; i < s_ncmds; i++) {
        if (s_cmds[i].cmd == c) {
            return true;
        }
    }
    return false;
}

static bool visible(lv_obj_t *o)
{
    return lv_obj_is_visible(o);
}

static void publish(const ui_model_t *m, uint32_t settle_ms)
{
    ui_model_publish(m);
    run_ms(settle_ms);
}

/* ---- concurrency: publish from another thread ------------------------------ */

static volatile bool s_stop;
static volatile uint32_t s_published;

static void *publisher(void *arg)
{
    (void)arg;
    ui_model_t m = model_playing();
    for (uint32_t i = 0; !s_stop; i++) {
        m.position_ms = i * 10 % 245000;
        snprintf(m.title, sizeof(m.title), "Titel %u – Größe", (unsigned)(i % 7));
        m.cover_id = i / 50; /* new cover now and then */
        m.toast_id = i / 100;
        snprintf(m.toast, sizeof(m.toast), "Läuft");
        ui_model_publish(&m);
        s_published = i + 1;
    }
    return NULL;
}

static lv_obj_tree_walk_res_t count_cb(lv_obj_t *o, void *ud)
{
    (void)o;
    (*(uint32_t *)ud)++;
    return LV_OBJ_TREE_WALK_NEXT;
}

/* ---- main -------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 1) {
        s_out = argv[1];
    }
    mkdir(s_out, 0755);
    setenv("TZ", "Europe/Berlin", 1);

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_cap_disp = lv_display_create(W, CAP_H);
    lv_display_set_color_format(s_cap_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_cap_disp, s_cap_fb, NULL, sizeof(s_cap_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_cap_disp, flush_cb);
    lv_obj_t *cs = lv_display_get_screen_active(s_cap_disp);
    lv_obj_set_style_bg_color(cs, lv_color_hex(0x1C1A17), 0);
    s_cap_label = lv_label_create(cs);
    lv_obj_set_style_text_font(s_cap_label, &ui_font_barlow_14, 0);
    lv_obj_set_style_text_color(s_cap_label, lv_color_hex(0xB5AB98), 0);
    lv_obj_align(s_cap_label, LV_ALIGN_LEFT_MID, 2, 0);

    s_disp = lv_display_create(W, H);
    lv_display_set_default(s_disp);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_disp);

    make_cover();
    ui_set_command_handler(cmd_cb, NULL);
    sim_mem_stats_t m0 = g_sim_mem;
    ui_init(s_disp);
    uint32_t nobj = 0;
    lv_obj_tree_walk(lv_display_get_screen_active(s_disp), count_cb, &nobj);
    printf("ui_init: %u LVGL objects; LVGL heap: %ld bytes in %ld blocks <= 4 kB (internal RAM on the device), "
           "%ld bytes in larger blocks (PSRAM); 64-bit host sizes\n",
           (unsigned)nobj, g_sim_mem.small_bytes - m0.small_bytes, g_sim_mem.small_blocks - m0.small_blocks,
           g_sim_mem.large_bytes - m0.large_bytes);
    ui_set_screensaver(0);

    /* ---------------- screens ---------------- */
    printf("screens -> %s\n", s_out);
    ui_model_t m = model_base();
    publish(&m, 200);
    snap("boot", "BOOT: Start");

    m = model_base();
    m.state = UI_STATE_SETUP_WIFI;
    m.wifi_ok = false;
    snprintf(m.setup_qr, sizeof(m.setup_qr), "WIFI:T:nopass;S:Music-Streamer;;");
    snprintf(m.setup_ssid, sizeof(m.setup_ssid), "Music-Streamer");
    snprintf(m.setup_url, sizeof(m.setup_url), "http://192.168.4.1");
    publish(&m, 500);
    snap("setup_wifi", "SETUP_WIFI: QR-Code, Hotspot-Name, Adresse");

    m = model_base();
    m.state = UI_STATE_CONNECTING_WIFI;
    m.wifi_ok = false;
    publish(&m, 900);
    snap("connecting", "CONNECTING_WIFI");

    m = model_base();
    m.state = UI_STATE_SETUP_SPOTIFY;
    snprintf(m.setup_qr, sizeof(m.setup_qr), "http://192.168.178.23/login");
    snprintf(m.setup_url, sizeof(m.setup_url), "192.168.178.23/login");
    m.setup_cancelable = true;
    publish(&m, 500);
    snap("setup_spotify", "SETUP_SPOTIFY: Login-QR (mit Abbrechen)");

    m = model_base();
    m.state = UI_STATE_WAIT_SPOTIFY;
    publish(&m, 500);
    snap("wait_spotify", "WAIT_SPOTIFY: Gerät in Spotify wählen");

    m = model_base();
    m.state = UI_STATE_IDLE;
    publish(&m, 500);
    snap("idle", "IDLE: Uhr, Tag auflegen");

    m = model_playing();
    publish(&m, 1500);
    snap("playing", "PLAYING mit Cover");

    m.tag_present = true;
    m.shuffle = true;
    m.wifi_ok = false;
    publish(&m, 300);
    snap("playing_status", "PLAYING: Zufall, Tag liegt auf, WLAN weg");

    m = model_playing();
    m.state = UI_STATE_PAUSED;
    m.playing = false;
    snprintf(m.title, sizeof(m.title), "Über sieben Brücken musst du gehn (Live in Köln, 1985)");
    snprintf(m.artist, sizeof(m.artist), "Karat & Peter Maffay feat. Herbert Grönemeyer");
    snprintf(m.context, sizeof(m.context), "Lieblingslieder für Sonntagmorgen");
    m.cover_rgb565 = NULL;
    m.position_ms = 61000;
    m.duration_ms = 312000;
    publish(&m, 500);
    snap("paused_no_cover", "PAUSED, ohne Cover, langer Titel mit Umlauten");
    run_ms(5500);
    snap("marquee", "Langer Titel läuft durch (circular)");

    m = model_playing();
    publish(&m, 300);
    m.toast_id = 1;
    m.toast_kind = UI_TOAST_NFC;
    snprintf(m.toast, sizeof(m.toast), "Playlist „Sommerhits 2026“ wird gestartet …");
    publish(&m, 400);
    snap("toast_nfc", "Hinweis: Tag erkannt");
    run_ms(5000);
    m.toast_id = 2;
    m.toast_kind = UI_TOAST_WARN;
    snprintf(m.toast, sizeof(m.toast), "Tag ohne Spotify-Link");
    publish(&m, 400);
    snap("toast_warn", "Hinweis: Tag ohne Spotify-Link");
    run_ms(8000);

    ui_settings_open();
    run_ms(500);
    snap("settings", "Einstellungen (oben)");
    m.update_available = true;
    snprintf(m.update_version, sizeof(m.update_version), "1.1.0");
    m.tag_remove_action = UI_TAG_REMOVE_STOP_AFTER_TRACK;
    publish(&m, 100);
    lv_obj_scroll_to_view_recursive(g_ui.set_upd_btn, LV_ANIM_OFF);
    run_ms(300);
    snap("settings_mid", "Einstellungen: WLAN, Update verfügbar");
    lv_obj_scroll_to_y(g_ui.set_list, 2000, LV_ANIM_OFF);
    run_ms(300);
    snap("settings_end", "Einstellungen: Tags, Gerät");
    ui_settings_close();
    run_ms(400);

    m = model_playing();
    m.state = UI_STATE_UPDATE;
    m.update_progress = 42;
    snprintf(m.update_text, sizeof(m.update_text), "Version 1.1.0 wird geladen – bitte nicht ausschalten");
    publish(&m, 500);
    snap("update", "UPDATE: Fortschritt");

    m = model_base();
    m.state = UI_STATE_ERROR;
    snprintf(m.error_title, sizeof(m.error_title), "Spotify nicht erreichbar");
    snprintf(m.error_text, sizeof(m.error_text), "Neuer Versuch in 30 Sekunden. Prüfe sonst, ob das WLAN funktioniert.");
    publish(&m, 500);
    snap("error", "ERROR");

    m = model_playing();
    publish(&m, 400);
    ui_set_screensaver(5);
    run_ms(6000);
    snap("screensaver", "Bildschirmschoner (Einbrennschutz)");
    m.state = UI_STATE_PAUSED;
    m.playing = false;
    publish(&m, 400);
    snap("screensaver_paused", "Bildschirmschoner: pausiert");
    m = model_base();
    m.state = UI_STATE_IDLE;
    publish(&m, 400);
    run_ms(21000);
    snap("screensaver_idle", "Bildschirmschoner: nichts läuft");
    ui_set_screensaver(0);
    tap(200, 200); /* wake */

    write_overview();

    /* ---------------- touch tests ---------------- */
    printf("touch tests\n");
    m = model_playing();
    publish(&m, 400);

    clear_cmds();
    touch_down(400, 100);
    touch_move(220, 104, 200);
    touch_up();
    expect(got1(UI_CMD_NEXT, 0), "swipe left -> NEXT");

    clear_cmds();
    touch_down(240, 100);
    touch_move(420, 96, 200);
    touch_up();
    expect(got1(UI_CMD_PREV, 0), "swipe right -> PREV");

    clear_cmds();
    touch_down(250, 70);
    touch_move(270, 200, 200);
    touch_up();
    expect(s_ncmds == 0, "vertical drag -> no command");

    clear_cmds();
    tap(110, 120);
    expect(got1(UI_CMD_TOGGLE, 0), "tap on cover -> TOGGLE");
    expect(g_ui.model.state == UI_STATE_PAUSED, "cover tap shows paused state at once");
    clear_cmds();
    tap(333, 170);
    expect(got1(UI_CMD_TOGGLE, 0), "tap on play button -> TOGGLE");
    clear_cmds();
    tap(410, 170);
    expect(got1(UI_CMD_NEXT, 0), "tap on next button -> NEXT");
    clear_cmds();
    tap(256, 170);
    expect(got1(UI_CMD_PREV, 0), "tap on prev button -> PREV");

    clear_cmds();
    tap(422, 32);
    expect(got1(UI_CMD_OPEN_SETTINGS, 0) && g_ui.settings_open && visible(g_ui.settings),
           "gear -> OPEN_SETTINGS, settings shown");

    clear_cmds();
    lv_area_t sa;
    lv_obj_get_coords(g_ui.set_slider, &sa);
    touch_down(sa.x1 + 20, (sa.y1 + sa.y2) / 2);
    touch_move(sa.x2 + 10, (sa.y1 + sa.y2) / 2, 300);
    touch_up();
    expect(s_ncmds > 0 && s_cmds[s_ncmds - 1].cmd == UI_CMD_SET_BRIGHTNESS && s_cmds[s_ncmds - 1].arg == 100,
           "brightness slider -> SET_BRIGHTNESS(100)");
    touch_down(sa.x2 - 2, (sa.y1 + sa.y2) / 2);
    touch_move(sa.x1 - 30, (sa.y1 + sa.y2) / 2, 300);
    touch_up();
    expect(s_cmds[s_ncmds - 1].cmd == UI_CMD_SET_BRIGHTNESS && s_cmds[s_ncmds - 1].arg == 5,
           "brightness slider -> SET_BRIGHTNESS(5), never below 5");

    clear_cmds();
    tap_obj(g_ui.set_seg[2]);
    expect(got1(UI_CMD_SET_TAG_REMOVE_ACTION, UI_TAG_REMOVE_STOP_AFTER_TRACK), "segment 3 -> SET_TAG_REMOVE_ACTION(2)");
    clear_cmds();
    tap_obj(g_ui.set_seg[1]);
    expect(got1(UI_CMD_SET_TAG_REMOVE_ACTION, UI_TAG_REMOVE_CONTINUE), "segment 2 -> SET_TAG_REMOVE_ACTION(1)");

    clear_cmds();
    int32_t y0 = lv_obj_get_scroll_y(g_ui.set_list);
    touch_down(300, 250);
    touch_move(300, 90, 300);
    touch_up();
    run_ms(800);
    expect(s_ncmds == 0 && lv_obj_get_scroll_y(g_ui.set_list) > y0 + 80, "drag up scrolls the settings list, no command");

    scroll_into_view(g_ui.set_shuffle);
    clear_cmds();
    tap_obj(g_ui.set_shuffle);
    expect(got1(UI_CMD_SHUFFLE, 1), "shuffle switch -> SHUFFLE(1)");
    clear_cmds();
    tap_obj(g_ui.set_shuffle);
    expect(got1(UI_CMD_SHUFFLE, 0), "shuffle switch again -> SHUFFLE(0)");

    scroll_into_view(g_ui.set_upd_btn);
    clear_cmds();
    tap_obj(g_ui.set_upd_btn);
    expect(got1(UI_CMD_CHECK_UPDATE, 0), "Prüfen -> CHECK_UPDATE");
    clear_cmds();
    tap_obj(g_ui.set_upd_btn);
    expect(s_ncmds == 0, "second tap while searching -> nothing");
    m.update_available = true;
    snprintf(m.update_version, sizeof(m.update_version), "1.1.0");
    publish(&m, 100);
    clear_cmds();
    tap_obj(g_ui.set_upd_btn);
    expect(got1(UI_CMD_START_UPDATE, 0), "Installieren -> START_UPDATE");
    m.update_available = false;

    lv_obj_t *tag_btn = lv_obj_get_child(lv_obj_get_parent(g_ui.set_tag_sub), -1);
    scroll_into_view(tag_btn);
    clear_cmds();
    tap_obj(tag_btn);
    expect(s_ncmds == 0 && g_ui.banner_on, "Tag speichern (not supported) -> hint only");
    tap_obj(g_ui.banner);
    expect(!g_ui.banner_on, "tap on toast hides it");
    m.tag_write_supported = true;
    publish(&m, 100);
    clear_cmds();
    tap_obj(tag_btn);
    expect(got2(UI_CMD_CLOSE_SETTINGS, UI_CMD_WRITE_TAG) && !g_ui.settings_open, "Tag speichern -> WRITE_TAG");

    ui_settings_open();
    run_ms(400);
    lv_obj_t *wifi_btn = lv_obj_get_child(lv_obj_get_parent(g_ui.set_wifi_sub), -1);
    scroll_into_view(wifi_btn);
    clear_cmds();
    tap_obj(wifi_btn);
    expect(got2(UI_CMD_CLOSE_SETTINGS, UI_CMD_OPEN_SETUP), "WLAN Ändern -> CLOSE_SETTINGS, OPEN_SETUP");

    run_ms(5000); /* "Leeren Tag auflegen" toast covers the top */
    clear_cmds();
    touch_down(300, 100);
    run_ms(900);
    touch_up();
    run_ms(400);
    expect(got1(UI_CMD_OPEN_SETTINGS, 0) && g_ui.settings_open, "long press on player -> settings");
    clear_cmds();
    tap(80, 32);
    expect(got1(UI_CMD_CLOSE_SETTINGS, 0) && !g_ui.settings_open && visible(g_ui.player), "Zurück -> CLOSE_SETTINGS");

    m = model_base();
    m.state = UI_STATE_SETUP_WIFI;
    m.setup_cancelable = true;
    snprintf(m.setup_qr, sizeof(m.setup_qr), "WIFI:T:nopass;S:Music-Streamer;;");
    publish(&m, 400);
    clear_cmds();
    tap_obj(g_ui.setup_cancel);
    expect(got1(UI_CMD_CANCEL, 0), "setup Abbrechen -> CANCEL");

    m = model_base();
    m.state = UI_STATE_ERROR;
    publish(&m, 400);
    clear_cmds();
    tap(422, 32);
    expect(got1(UI_CMD_OPEN_SETTINGS, 0) && visible(g_ui.settings), "gear on error screen -> settings");
    m.state = UI_STATE_UPDATE;
    publish(&m, 400);
    expect(!g_ui.settings_open && any(UI_CMD_CLOSE_SETTINGS) && visible(g_ui.ota), "update state closes settings");

    m = model_base();
    m.state = UI_STATE_IDLE;
    publish(&m, 400);
    clear_cmds();
    tap(422, 32);
    expect(got1(UI_CMD_OPEN_SETTINGS, 0), "gear on idle screen -> settings");
    run_ms(61000);
    expect(!g_ui.settings_open, "settings close after 60 s without touch");

    m = model_playing();
    publish(&m, 400);
    ui_set_screensaver(2);
    run_ms(2500);
    expect(g_ui.saver_on && visible(g_ui.saver), "screensaver after timeout");
    clear_cmds();
    tap(110, 120);
    expect(!g_ui.saver_on && s_ncmds == 0 && visible(g_ui.player), "touch wakes without command");
    ui_set_screensaver(0);

    long heap_before = g_sim_mem.small_bytes;
    printf("concurrency test\n");
    pthread_t th;
    s_stop = false;
    pthread_create(&th, NULL, publisher, NULL);
    for (int i = 0; i < 400; i++) {
        run_ms(10);
        lv_refr_now(s_disp);
    }
    for (int i = 0; i < 10000 && s_published < 1000; i++) {
        run_ms(10); /* make sure the thread really ran (busy build machines) */
        usleep(1000);
    }
    s_stop = true;
    pthread_join(th, NULL);
    run_ms(100);
    printf("  %u publishes, last title: %s\n", (unsigned)s_published, g_ui.model.title);
    expect(strncmp(g_ui.model.title, "Titel", 5) == 0, "publish from another thread while rendering");
    expect(g_sim_mem.small_bytes - heap_before < 2048, "no LVGL heap growth over thousands of updates");

    printf("LVGL heap at the end: %ld bytes <= 4 kB (peak %ld), %ld bytes larger\n", g_sim_mem.small_bytes,
           g_sim_mem.small_peak, g_sim_mem.large_bytes);
    printf("%s\n", s_fail ? "FAILED" : "all tests passed");
    return s_fail ? 1 : 0;
}
