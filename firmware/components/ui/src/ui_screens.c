/*
 * ui_screens.c - builds and updates the LVGL objects of all screens.
 * LVGL task only (called from ui.c).
 *
 * Layout (landscape 456 x 280, "Richtung C"): cover or record left, text
 * column right, brass bar with progress at the bottom, settings gear top
 * right. All screens are built once in uis_build() and only shown/hidden;
 * a black fader object cross-fades between them (cheap: one blended rect).
 * Touch targets are at least 56 x 56 px, the transport buttons 76 x 76.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_internal.h"
#include "ui_port.h"

#define F14 (&ui_font_barlow_14)
#define F18 (&ui_font_barlow_18)
#define F24 (&ui_font_barlow_24)
#define F32 (&ui_font_barlow_32)
#define F56 (&ui_font_barlow_56)
#define F104 (&ui_font_barlow_104)

/* player transport row: three 76 x 76 zones */
#define BTN_Y   132
#define BTN_SZ  76
#define BTN_X0  218
#define TRACK_X 78
#define TRACK_W (UI_W - 2 * TRACK_X)

/* settings list */
#define SET_HEAD 64
#define SET_PAD  24
#define SET_W    (UI_W - 2 * SET_PAD)

/* screensaver: everything dim and warm (AMOLED burn-in), moved every 20 s */
#define SAVER_CLOCK    0x8F8676   /* clock: the brightest thing on the screen */
#define SAVER_TEXT     0x8A8274
#define SAVER_COL      0x5A554B
#define SAVER_BRASS    0x6B5A36   /* brass at ~half brightness */
#define SAVER_THUMB    56
#define SAVER_TEXT_MAX 230        /* title/artist column, longer text ends in "…" */
#define SAVER_DRIFT_X  18         /* the card sits around the screen centre and */
#define SAVER_DRIFT_Y  14         /* moves by up to this much every 20 s        */

/* ======================================================================
 * helpers
 * ====================================================================== */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *layer(lv_obj_t *parent, bool clickable)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (clickable) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t w)
{
    lv_obj_t *l = label(parent, font, color, "");
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(l, 2, 0);
    return l;
}

static void set_text_if(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void caps_set(lv_obj_t *l, const char *text)
{
    char buf[128];
    ui_utf8_upper(text, buf, sizeof(buf));
    set_text_if(l, buf);
}

/* brass small caps, letter-spaced (lettering of an amp's control panel) */
static lv_obj_t *caps(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = label(parent, F14, COL_BRASS, "");
    lv_obj_set_style_text_letter_space(l, 2, 0);
    caps_set(l, text);
    return l;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *dsc, uint32_t color)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(i, dsc);
    lv_obj_set_style_image_recolor(i, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    return i;
}

static void icon_set(lv_obj_t *i, const lv_image_dsc_t *dsc)
{
    if (lv_image_get_src(i) != dsc) {
        lv_image_set_src(i, dsc);
    }
}

static void show(lv_obj_t *o, bool visible)
{
    if (!o || visible != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *circle(lv_obj_t *parent, int32_t d, uint32_t color)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

static void brass_fill(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_BRASS_HI), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(COL_BRASS_LO), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
}

/* brushed-brass bar along the bottom edge */
static lv_obj_t *brass_bar(lv_obj_t *parent)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_pos(b, 0, BAR_Y);
    lv_obj_set_size(b, UI_W, BAR_H);
    brass_fill(b);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xEFD9A6), 0);
    return b;
}

/* pill-shaped outline button; effective height 48 + 2 * 10 = 68 px */
static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, int32_t w, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = box(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_BRASS), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL_HI), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_t *l = label(b, F18, COL_CREAM, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (lbl_out) {
        *lbl_out = l;
    }
    return b;
}

static lv_obj_t *flex_col(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, gap, 0);
    return c;
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

/* settings gear, top right: 56 x 56 circle (+6 px touch margin), quiet grey icon */
static lv_obj_t *gear_button(lv_obj_t *parent)
{
    lv_obj_t *b = box(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(b, GEAR_X + 2, 4);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL_HI), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *i = icon(b, &ui_icon_gear_28, COL_GREY);
    lv_obj_center(i);
    lv_obj_add_event_cb(b, gear_cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void ui_utf8_upper(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    if (!src) {
        src = "";
    }
    for (const unsigned char *s = (const unsigned char *)src; *s && o + 3 < cap;) {
        unsigned char c = s[0];
        if (c < 0x80) {
            dst[o++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
            s++;
        }
        else if (c == 0xC3 && s[1]) {
            unsigned char d = s[1];
            /* à..þ -> À..Þ, except ÷; ß and ÿ stay */
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) {
                d -= 0x20;
            }
            dst[o++] = (char)c;
            dst[o++] = (char)d;
            s += 2;
        }
        else if ((c == 0xC4 || c == 0xC5) && s[1]) {
            unsigned cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
            if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) {
                cp &= ~1u; /* even = upper case */
            }
            else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
                if (!(cp & 1u)) {
                    cp -= 1; /* odd = upper case */
                }
            }
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
            s += 2;
        }
        else {
            /* copy any other sequence unchanged */
            int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (o + (size_t)n >= cap) {
                break;
            }
            for (int k = 0; k < n && s[0]; k++) {
                dst[o++] = (char)*s++;
            }
        }
    }
    dst[o] = '\0';
}

void ui_fmt_time(char *buf, size_t cap, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(buf, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    }
    else {
        snprintf(buf, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

/* wall clock; false until SNTP has set a plausible time */
static bool clock_now(struct tm *out)
{
    time_t t = time(NULL);
    localtime_r(&t, out);
    return out->tm_year + 1900 >= 2024;
}

static const char *dev_name(void)
{
    return g_ui.model.device_name[0] ? g_ui.model.device_name : "Music Streamer";
}

static const char *rssi_text(int8_t rssi)
{
    return rssi >= -60 ? "sehr gut" : rssi >= -67 ? "gut" : rssi >= -75 ? "mäßig" : "schwach";
}

/* ======================================================================
 * marquee with soft edges
 * ====================================================================== */

static lv_anim_t s_marquee;
static lv_obj_t *s_fade[2][2]; /* [title, artist][left, right] */

static lv_obj_t *fade(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, bool to_right)
{
    lv_obj_t *f = box(parent);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_size(f, w, h);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(f, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_main_opa(f, to_right ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(f, to_right ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_add_flag(f, LV_OBJ_FLAG_HIDDEN);
    return f;
}

static void marquee_text(lv_obj_t *l, int row, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) == 0) {
        return;
    }
    lv_label_set_text(l, text);
    const lv_font_t *font = lv_obj_get_style_text_font(l, 0);
    bool scrolls = lv_text_get_width(text, (uint32_t)strlen(text), font, 0) > lv_obj_get_width(l);
    show(s_fade[row][0], false);
    show(s_fade[row][1], scrolls);
}

/* left edge fades only while the text is moving, so the first letter is
 * crisp during the pause */
void uis_update_fades(void)
{
    lv_obj_t *labels[2] = {g_ui.lbl_title, g_ui.lbl_artist};
    for (int row = 0; row < 2; row++) {
        bool moving = false;
        if (!lv_obj_has_flag(s_fade[row][1], LV_OBJ_FLAG_HIDDEN)) {
            lv_anim_t *a = lv_anim_get(labels[row], NULL);
            moving = a && a->act_time > 0 && a->current_value != a->start_value &&
                     a->current_value != a->end_value;
        }
        show(s_fade[row][0], moving);
    }
}

static void marquee(lv_obj_t *l, int32_t w)
{
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_anim(l, &s_marquee, 0);
    lv_obj_set_style_anim_duration(l, lv_anim_speed_clamped(35, 2000, 10000), 0);
}

/* ======================================================================
 * player
 * ====================================================================== */

static lv_obj_t *transport_zone(lv_obj_t *parent, int idx)
{
    lv_obj_t *z = box(parent);
    lv_obj_set_pos(z, BTN_X0 + idx * (BTN_SZ + 1), BTN_Y);
    lv_obj_set_size(z, BTN_SZ, BTN_SZ);
    lv_obj_set_style_radius(z, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(z, lv_color_hex(COL_PANEL_HI), 0);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    return z;
}

static void build_player(void)
{
    lv_obj_t *p = g_ui.player = layer(g_ui.scr, true);
    lv_obj_add_event_cb(p, ui_player_event_cb, LV_EVENT_ALL, NULL);

    /* cover (or record) */
    g_ui.cover_img = lv_image_create(p);
    lv_obj_remove_flag(g_ui.cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(g_ui.cover_img, COVER_X, COVER_Y);
    lv_obj_set_size(g_ui.cover_img, COVER, COVER);
    lv_image_set_inner_align(g_ui.cover_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_set_style_radius(g_ui.cover_img, 10, 0);
    lv_obj_set_style_clip_corner(g_ui.cover_img, true, 0);
    lv_image_set_src(g_ui.cover_img, &ui_img_vinyl_180);

    /* paused: darkened cover with a play symbol */
    g_ui.cover_shade = box(p);
    lv_obj_set_pos(g_ui.cover_shade, COVER_X, COVER_Y);
    lv_obj_set_size(g_ui.cover_shade, COVER, COVER);
    lv_obj_set_style_radius(g_ui.cover_shade, 10, 0);
    lv_obj_set_style_bg_color(g_ui.cover_shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_ui.cover_shade, LV_OPA_50, 0);
    lv_obj_t *ring = circle(g_ui.cover_shade, 76, COL_BG);
    lv_obj_set_style_bg_opa(ring, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COL_CREAM), 0);
    lv_obj_center(ring);
    lv_obj_t *pi = icon(ring, &ui_icon_play_48, COL_CREAM);
    lv_obj_align(pi, LV_ALIGN_CENTER, 3, 0);

    /* text column */
    lv_obj_t *c = g_ui.col = box(p);
    lv_obj_set_pos(c, COL_X, 26);
    lv_obj_set_size(c, COL_W, 100);
    g_ui.lbl_context = caps(c, "");
    lv_obj_set_pos(g_ui.lbl_context, 0, 4);
    lv_obj_set_size(g_ui.lbl_context, COL_W - 48, 18);
    lv_label_set_long_mode(g_ui.lbl_context, LV_LABEL_LONG_MODE_DOTS);
    g_ui.lbl_title = label(c, F32, COL_CREAM, "");
    lv_obj_set_pos(g_ui.lbl_title, 0, 22);
    marquee(g_ui.lbl_title, COL_W);
    g_ui.lbl_artist = label(c, F18, COL_GREY, "");
    lv_obj_set_pos(g_ui.lbl_artist, 0, 66);
    marquee(g_ui.lbl_artist, COL_W);
    s_fade[0][0] = fade(c, 0, 22, 16, 42, false);
    s_fade[0][1] = fade(c, COL_W - 28, 22, 28, 42, true);
    s_fade[1][0] = fade(c, 0, 66, 16, 24, false);
    s_fade[1][1] = fade(c, COL_W - 28, 66, 28, 24, true);

    /* status icons left of the gear, only when there is something to say */
    lv_obj_t *s = g_ui.status_box = box(p);
    lv_obj_set_size(s, LV_SIZE_CONTENT, 20);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s, 8, 0);
    lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -GEAR_W + 2, 22);
    g_ui.st_shuffle = icon(s, &ui_icon_shuffle_20, COL_GREY);
    g_ui.st_nfc = icon(s, &ui_icon_nfc_20, COL_BRASS);
    g_ui.st_wifi = icon(s, &ui_icon_wifi_off_20, COL_ERR);

    gear_button(p);

    /* transport: prev | play/pause | next */
    g_ui.btn_prev = transport_zone(p, 0);
    lv_obj_center(icon(g_ui.btn_prev, &ui_icon_prev_32, COL_CREAM));
    g_ui.btn_play = transport_zone(p, 1);
    lv_obj_t *pc = circle(g_ui.btn_play, 64, COL_BRASS);
    brass_fill(pc);
    lv_obj_center(pc);
    g_ui.ico_play = icon(pc, &ui_icon_pause_32, COL_INK);
    lv_obj_center(g_ui.ico_play);
    g_ui.btn_next = transport_zone(p, 2);
    lv_obj_center(icon(g_ui.btn_next, &ui_icon_next_32, COL_CREAM));

    /* brass bar with progress */
    lv_obj_t *bar = brass_bar(p);
    g_ui.lbl_pos = label(bar, F18, COL_INK, "0:00");
    lv_obj_align(g_ui.lbl_pos, LV_ALIGN_LEFT_MID, 20, 0);
    g_ui.lbl_dur = label(bar, F18, COL_INK, "0:00");
    lv_obj_align(g_ui.lbl_dur, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_t *track = box(bar);
    lv_obj_set_size(track, TRACK_W, 4);
    lv_obj_align(track, LV_ALIGN_LEFT_MID, TRACK_X, 0);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(COL_BRASS_DK), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    g_ui.bar_fill = box(track);
    lv_obj_set_size(g_ui.bar_fill, 0, 4);
    lv_obj_set_style_radius(g_ui.bar_fill, 2, 0);
    lv_obj_set_style_bg_color(g_ui.bar_fill, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_bg_opa(g_ui.bar_fill, LV_OPA_COVER, 0);
}

void uis_set_cover(uint16_t *px, uint16_t w, uint16_t h)
{
    uint16_t *old = g_ui.cover_buf;
    lv_image_dsc_t *old_dsc = &g_ui.cover_dsc[g_ui.cover_dsc_idx];
    if (px) {
        g_ui.cover_dsc_idx ^= 1;
        lv_image_dsc_t *d = &g_ui.cover_dsc[g_ui.cover_dsc_idx];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = w;
        d->header.h = h;
        d->header.stride = (uint32_t)w * 2;
        d->data_size = (uint32_t)w * h * 2;
        d->data = (const uint8_t *)px;
        lv_image_set_src(g_ui.cover_img, d);
        lv_image_set_src(g_ui.saver_cover, d);
    }
    else {
        lv_image_set_src(g_ui.cover_img, &ui_img_vinyl_180);
        lv_image_set_src(g_ui.saver_cover, &ui_img_vinyl_180);
    }
    g_ui.cover_buf = px;
    if (old) {
        lv_image_cache_drop(old_dsc);
        ui_port_free(old);
    }
}

static void update_player(void)
{
    const ui_model_t *m = &g_ui.model;
    bool playing = m->state == UI_STATE_PLAYING;
    show(g_ui.lbl_context, m->context[0] != '\0');
    caps_set(g_ui.lbl_context, m->context);
    marquee_text(g_ui.lbl_title, 0, m->title[0] ? m->title : "…");
    marquee_text(g_ui.lbl_artist, 1, m->artist);

    icon_set(g_ui.ico_play, playing ? &ui_icon_pause_32 : &ui_icon_play_32);
    lv_obj_align(g_ui.ico_play, LV_ALIGN_CENTER, playing ? 0 : 2, 0);
    show(g_ui.cover_shade, !playing);

    show(g_ui.st_shuffle, m->shuffle);
    show(g_ui.st_nfc, m->tag_present);
    show(g_ui.st_wifi, !m->wifi_ok);
    int icons = (m->shuffle ? 1 : 0) + (m->tag_present ? 1 : 0) + (m->wifi_ok ? 0 : 1);
    lv_obj_set_width(g_ui.lbl_context, COL_W - 48 - icons * 28);
}

void uis_update_progress(bool force)
{
    const ui_model_t *m = &g_ui.model;
    if (g_ui.view != VIEW_PLAYER) {
        return;
    }
    uint32_t pos = g_ui.pos_ms_at_tick + (m->state == UI_STATE_PLAYING ? ui_now() - g_ui.pos_tick : 0);
    if (m->duration_ms && pos > m->duration_ms) {
        pos = m->duration_ms;
    }
    int ps = (int)(pos / 1000), ds = (int)(m->duration_ms / 1000);
    char buf[16];
    if (force || ps != g_ui.last_pos_s) {
        g_ui.last_pos_s = ps;
        ui_fmt_time(buf, sizeof(buf), pos);
        set_text_if(g_ui.lbl_pos, buf);
    }
    if (force || ds != g_ui.last_dur_s) {
        g_ui.last_dur_s = ds;
        if (m->duration_ms) {
            ui_fmt_time(buf, sizeof(buf), m->duration_ms);
        }
        else {
            snprintf(buf, sizeof(buf), "–:––");
        }
        set_text_if(g_ui.lbl_dur, buf);
    }
    int32_t w = m->duration_ms ? (int32_t)((uint64_t)TRACK_W * pos / m->duration_ms) : 0;
    if (lv_obj_get_width(g_ui.bar_fill) != w) {
        lv_obj_set_width(g_ui.bar_fill, w);
    }
}

ui_zone_t uis_zone_at(lv_point_t p)
{
    const int32_t slack = 8;
    if (p.x >= COVER_X - slack && p.x < COVER_X + COVER + slack && p.y >= COVER_Y - slack &&
        p.y < COVER_Y + COVER + slack) {
        return ZONE_COVER;
    }
    if (p.y >= BTN_Y - slack && p.y < BTN_Y + BTN_SZ + slack && p.x >= BTN_X0) {
        int idx = (p.x - BTN_X0) / (BTN_SZ + 1);
        return idx <= 0 ? ZONE_PREV : idx == 1 ? ZONE_PLAY : ZONE_NEXT;
    }
    return ZONE_NONE;
}

void uis_zone_feedback(ui_zone_t z, bool pressed)
{
    lv_opa_t opa = pressed ? LV_OPA_COVER : LV_OPA_TRANSP;
    switch (z) {
    case ZONE_COVER:
        lv_obj_set_style_image_opa(g_ui.cover_img, pressed ? LV_OPA_60 : LV_OPA_COVER, 0);
        break;
    case ZONE_PREV:
        lv_obj_set_style_bg_opa(g_ui.btn_prev, opa, 0);
        break;
    case ZONE_PLAY:
        lv_obj_set_style_bg_opa(g_ui.btn_play, opa, 0);
        break;
    case ZONE_NEXT:
        lv_obj_set_style_bg_opa(g_ui.btn_next, opa, 0);
        break;
    default:
        break;
    }
}

/* text column follows the finger a little while swiping */
static int32_t s_swipe_from;

static void col_anim_cb(void *var, int32_t v)
{
    (void)var;
    /* v: 0..256 progress from s_swipe_from back to 0 */
    int32_t x = s_swipe_from * (256 - v) / 256;
    lv_obj_set_style_translate_x(g_ui.col, x, 0);
    int32_t opa = 255 - LV_ABS(x) * 3;
    lv_obj_set_style_opa(g_ui.col, (lv_opa_t)LV_CLAMP(40, opa, 255), 0);
}

void uis_drag(int32_t dx)
{
    lv_anim_delete(&s_swipe_from, col_anim_cb);
    s_swipe_from = LV_CLAMP(-70, dx / 2, 70);
    col_anim_cb(NULL, 0);
}

void uis_swipe_done(int dir)
{
    lv_anim_delete(&s_swipe_from, col_anim_cb);
    if (dir != 0) {
        /* new title slides in from the side the finger moved away from */
        s_swipe_from = dir < 0 ? 60 : -60;
    }
    else {
        s_swipe_from = lv_obj_get_style_translate_x(g_ui.col, 0);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_swipe_from);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, col_anim_cb);
    lv_anim_start(&a);
}

/* ======================================================================
 * hint: connecting, waiting for Spotify, idle
 * ====================================================================== */

static void build_hint(void)
{
    lv_obj_t *h = g_ui.hint = layer(g_ui.scr, true);

    lv_obj_t *v = lv_image_create(h);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(v, &ui_img_vinyl_180);
    lv_obj_set_pos(v, COVER_X, COVER_Y);

    /* thin brass arc running around the record while connecting */
    lv_obj_t *sp = g_ui.hint_spinner = lv_spinner_create(h);
    lv_obj_remove_style_all(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, COVER + 16, COVER + 16);
    lv_obj_set_pos(sp, COVER_X - 8, COVER_Y - 8);
    lv_obj_set_style_arc_width(sp, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, lv_color_hex(COL_BRASS), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(sp, true, LV_PART_INDICATOR);
    lv_spinner_set_anim_params(sp, 1600, 60);

    lv_obj_t *c = flex_col(h, 6);
    lv_obj_set_pos(c, COL_X, 30);
    lv_obj_set_size(c, COL_W, BAR_Y - 30 - 8);
    g_ui.hint_caps = caps(c, "");
    g_ui.hint_title = label(c, F32, COL_CREAM, "");
    g_ui.hint_clock_big = label(c, F56, COL_CREAM, "");
    lv_obj_set_style_margin_bottom(g_ui.hint_clock_big, 2, 0);

    lv_obj_t *row = g_ui.hint_tag_row = box(c);
    lv_obj_set_size(row, COL_W, 30);
    lv_obj_t *ni = icon(row, &ui_icon_nfc_20, COL_BRASS);
    lv_obj_align(ni, LV_ALIGN_LEFT_MID, 0, 0);
    g_ui.hint_tag_lbl = label(row, F18, COL_CREAM, "");
    lv_obj_align(g_ui.hint_tag_lbl, LV_ALIGN_LEFT_MID, 30, 0);

    g_ui.hint_body = wrap_label(c, F18, COL_GREY, COL_W);

    gear_button(h);

    lv_obj_t *bar = brass_bar(h);
    g_ui.hint_name = label(bar, F14, COL_INK, "");
    lv_obj_set_style_text_letter_space(g_ui.hint_name, 3, 0);
    lv_obj_align(g_ui.hint_name, LV_ALIGN_CENTER, 0, 0);
    g_ui.hint_clock = label(bar, F18, COL_INK, "");
    lv_obj_align(g_ui.hint_clock, LV_ALIGN_RIGHT_MID, -20, 0);
}

static void update_hint(void)
{
    const ui_model_t *m = &g_ui.model;
    char buf[200];
    struct tm tm;
    bool clock = clock_now(&tm);
    bool idle_clock = m->state == UI_STATE_IDLE && clock;

    show(g_ui.hint_spinner, m->state == UI_STATE_CONNECTING_WIFI);
    if (m->state == UI_STATE_CONNECTING_WIFI) {
        caps_set(g_ui.hint_caps, "WLAN");
        set_text_if(g_ui.hint_title, "Verbinde …");
        if (m->wifi_ssid[0]) {
            snprintf(buf, sizeof(buf), "mit „%s“", m->wifi_ssid);
        }
        else {
            snprintf(buf, sizeof(buf), "Einen Moment bitte.");
        }
        set_text_if(g_ui.hint_body, buf);
        show(g_ui.hint_tag_row, false);
    }
    else if (m->state == UI_STATE_IDLE) {
        caps_set(g_ui.hint_caps, "Bereit");
        set_text_if(g_ui.hint_title, "Musik wählen");
        set_text_if(g_ui.hint_tag_lbl, "Tag auflegen");
        snprintf(buf, sizeof(buf), "oder in Spotify „%s“ wählen", dev_name());
        set_text_if(g_ui.hint_body, buf);
        show(g_ui.hint_tag_row, true);
    }
    else { /* WAIT_SPOTIFY */
        caps_set(g_ui.hint_caps, "Bereit");
        set_text_if(g_ui.hint_title, "Musik wählen");
        snprintf(buf, sizeof(buf), "Öffne Spotify auf dem Handy und wähle „%s“.", dev_name());
        set_text_if(g_ui.hint_body, buf);
        set_text_if(g_ui.hint_tag_lbl, "oder Tag auflegen");
        show(g_ui.hint_tag_row, true);
    }
    /* idle: "Tag auflegen" first; waiting: the Spotify hint first */
    int32_t want = m->state == UI_STATE_IDLE ? lv_obj_get_index(g_ui.hint_body) : -1;
    if (want >= 0 && lv_obj_get_index(g_ui.hint_tag_row) > want) {
        lv_obj_move_to_index(g_ui.hint_tag_row, want);
    }
    else if (want < 0 && lv_obj_get_index(g_ui.hint_tag_row) < lv_obj_get_index(g_ui.hint_body)) {
        lv_obj_move_to_index(g_ui.hint_tag_row, -1);
    }
    show(g_ui.hint_title, !idle_clock);
    show(g_ui.hint_clock_big, idle_clock);
    show(g_ui.hint_clock, !idle_clock);
    caps_set(g_ui.hint_name, dev_name());
}

static void saver_set_date(const struct tm *tm)
{
    static const char *const wd[] = { "Sonntag", "Montag", "Dienstag", "Mittwoch",
                                      "Donnerstag", "Freitag", "Samstag" };
    static const char *const mo[] = { "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
                                      "August", "September", "Oktober", "November", "Dezember" };
    char buf[48] = "";
    if (tm) {
        snprintf(buf, sizeof(buf), "%s, %d. %s", wd[tm->tm_wday % 7], tm->tm_mday, mo[tm->tm_mon % 12]);
    }
    set_text_if(g_ui.saver_date, buf);
}

void uis_update_clock(void)
{
    struct tm tm;
    bool ok = clock_now(&tm);
    int minute = ok ? tm.tm_hour * 60 + tm.tm_min : -2;
    if (minute == g_ui.last_clock_min) {
        return;
    }
    bool was_ok = g_ui.last_clock_min >= 0;
    g_ui.last_clock_min = minute;
    char buf[8] = "";
    if (ok) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour % 24, tm.tm_min % 60);
    }
    lv_label_set_text(g_ui.hint_clock, buf);
    lv_label_set_text(g_ui.hint_clock_big, buf);
    lv_label_set_text(g_ui.saver_clock, buf);
    saver_set_date(ok ? &tm : NULL);
    if (ok != was_ok) {
        update_hint(); /* clock became valid: idle screen switches to the big clock */
    }
}

/* ======================================================================
 * setup (QR), update, error, splash
 * ====================================================================== */

static void setup_cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_send_cmd(UI_CMD_CANCEL, 0);
}

static void build_setup(void)
{
    lv_obj_t *s = g_ui.setup = layer(g_ui.scr, true);

    lv_obj_t *card = box(s);
    lv_obj_set_pos(card, 16, 20);
    lv_obj_set_size(card, 200, 200);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CREAM), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    g_ui.qr = lv_qrcode_create(card);
    lv_qrcode_set_size(g_ui.qr, 176);
    lv_qrcode_set_dark_color(g_ui.qr, lv_color_black());
    lv_qrcode_set_light_color(g_ui.qr, lv_color_hex(COL_CREAM));
    lv_obj_center(g_ui.qr);

    g_ui.setup_note = label(s, F14, COL_GREY, "");
    lv_obj_set_size(g_ui.setup_note, 216, 40);
    lv_obj_set_style_text_align(g_ui.setup_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_ui.setup_note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(g_ui.setup_note, 8, 230);

    lv_obj_t *c = flex_col(s, 8);
    lv_obj_set_pos(c, 236, 22);
    lv_obj_set_size(c, UI_W - 236 - 16, 190);
    g_ui.setup_caps = caps(c, "");
    lv_obj_set_style_margin_bottom(g_ui.setup_caps, 4, 0);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = box(c);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        lv_obj_t *n = label(row, F24, COL_BRASS, num);
        lv_obj_set_pos(n, 0, -3);
        g_ui.setup_step[i] = wrap_label(row, F18, COL_CREAM, UI_W - 236 - 16 - 24);
        lv_obj_set_pos(g_ui.setup_step[i], 24, 0);
    }
    g_ui.setup_cancel = pill_button(s, "Abbrechen", 160, setup_cancel_cb, NULL);
    lv_obj_set_pos(g_ui.setup_cancel, 236, 220);
}

static void update_setup(void)
{
    const ui_model_t *m = &g_ui.model;
    char buf[128];
    if (m->state == UI_STATE_SETUP_SPOTIFY) {
        caps_set(g_ui.setup_caps, "Mit Spotify verbinden");
        set_text_if(g_ui.setup_step[0], "QR-Code mit dem Handy scannen");
        set_text_if(g_ui.setup_step[1], "Bei Spotify anmelden und zustimmen");
        set_text_if(g_ui.setup_step[2], "Fertig – jetzt spielen auch die Tags");
        snprintf(buf, sizeof(buf), "%s", m->setup_url);
    }
    else {
        caps_set(g_ui.setup_caps, "WLAN einrichten");
        set_text_if(g_ui.setup_step[0], "QR-Code mit dem Handy scannen");
        set_text_if(g_ui.setup_step[1], "Die Einrichtung öffnet sich");
        set_text_if(g_ui.setup_step[2], "Heim-WLAN wählen, Passwort eingeben");
        /* text fallback below the code: hotspot name and address */
        if (m->setup_ssid[0]) {
            snprintf(buf, sizeof(buf), "WLAN „%s“\n%s", m->setup_ssid, m->setup_url);
        }
        else {
            snprintf(buf, sizeof(buf), "%s", m->setup_url);
        }
    }
    set_text_if(g_ui.setup_note, buf);
    if (strcmp(g_ui.qr_shown, m->setup_qr) != 0) {
        memcpy(g_ui.qr_shown, m->setup_qr, sizeof(g_ui.qr_shown));
        const char *qr = m->setup_qr[0] ? m->setup_qr : " ";
        lv_qrcode_update(g_ui.qr, qr, (uint32_t)strlen(qr));
    }
    show(g_ui.setup_cancel, m->setup_cancelable);
}

static void build_ota(void)
{
    lv_obj_t *o = g_ui.ota = layer(g_ui.scr, true);
    lv_obj_t *i = icon(o, &ui_icon_update_56, COL_BRASS);
    lv_obj_align(i, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *t = caps(o, "Software-Update");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 82);
    g_ui.ota_pct = label(o, F56, COL_CREAM, "0 %");
    lv_obj_align(g_ui.ota_pct, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t *track = box(o);
    lv_obj_set_size(track, 280, 6);
    lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    g_ui.ota_fill = box(track);
    lv_obj_set_size(g_ui.ota_fill, 0, 6);
    lv_obj_set_style_radius(g_ui.ota_fill, 3, 0);
    lv_obj_set_style_bg_color(g_ui.ota_fill, lv_color_hex(COL_BRASS), 0);
    lv_obj_set_style_bg_opa(g_ui.ota_fill, LV_OPA_COVER, 0);
    g_ui.ota_note = label(o, F18, COL_GREY, "");
    lv_obj_set_width(g_ui.ota_note, UI_W - 40);
    lv_obj_set_style_text_align(g_ui.ota_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_ui.ota_note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(g_ui.ota_note, LV_ALIGN_TOP_MID, 0, 218);
}

static void update_ota(void)
{
    const ui_model_t *m = &g_ui.model;
    char buf[16];
    int p = m->update_progress > 100 ? 100 : m->update_progress;
    snprintf(buf, sizeof(buf), "%d %%", p);
    set_text_if(g_ui.ota_pct, buf);
    lv_obj_set_width(g_ui.ota_fill, 280 * p / 100);
    const char *note = m->update_text[0] ? m->update_text : "Bitte nicht ausschalten";
    if (m->update_progress > 100) {
        note = "Fertig – Neustart …";
    }
    set_text_if(g_ui.ota_note, note);
}

static void build_error(void)
{
    lv_obj_t *t = g_ui.error = layer(g_ui.scr, true);
    lv_obj_t *ring = circle(t, 136, COL_BG);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COL_LINE), 0);
    lv_obj_set_pos(ring, 44, 72);
    lv_obj_t *i = icon(ring, &ui_icon_warn_56, COL_ERR);
    lv_obj_center(i);

    lv_obj_t *c = flex_col(t, 6);
    lv_obj_set_pos(c, 212, 0);
    lv_obj_set_size(c, UI_W - 212 - 20, UI_H);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    caps(c, "Hinweis");
    g_ui.err_title = wrap_label(c, F24, COL_CREAM, LV_PCT(100));
    g_ui.err_text = wrap_label(c, F18, COL_GREY, LV_PCT(100));
    gear_button(t);
}

static void update_error(void)
{
    const ui_model_t *m = &g_ui.model;
    set_text_if(g_ui.err_title, m->error_title[0] ? m->error_title : "Etwas ist schiefgelaufen");
    set_text_if(g_ui.err_text, m->error_text);
    show(g_ui.err_text, m->error_text[0] != '\0');
}

static void build_splash(void)
{
    lv_obj_t *s = g_ui.splash = layer(g_ui.scr, false);
    lv_obj_t *v = lv_image_create(s);
    lv_image_set_src(v, &ui_img_vinyl_180);
    lv_obj_set_pos(v, COVER_X, (UI_H - COVER) / 2);
    lv_obj_t *t = label(s, F24, COL_CREAM, "MUSIC\nSTREAMER");
    lv_obj_set_style_text_letter_space(t, 6, 0);
    lv_obj_set_style_text_line_space(t, 4, 0);
    lv_obj_set_pos(t, COL_X + 8, 86);
    lv_obj_t *line = box(s);
    lv_obj_set_size(line, 56, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_BRASS), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_pos(line, COL_X + 8, 160);
    lv_obj_t *st = label(s, F14, COL_GREY, "startet …");
    lv_obj_set_style_text_letter_space(st, 1, 0);
    lv_obj_set_pos(st, COL_X + 8, 174);
}

/* ======================================================================
 * toast banner
 * ====================================================================== */

static void banner_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void banner_hidden_cb(lv_anim_t *a)
{
    (void)a;
    if (!g_ui.banner_on) {
        show(g_ui.banner, false);
    }
}

static void banner_tap_cb(lv_event_t *e)
{
    (void)e;
    uis_banner_hide();
}

static void build_banner(void)
{
    lv_obj_t *b = g_ui.banner = box(g_ui.scr);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(b, UI_W - 32, 52);
    lv_obj_set_pos(b, 16, -64);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1B1813), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(b, 24, 0);
    lv_obj_set_style_shadow_color(b, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(b, banner_tap_cb, LV_EVENT_CLICKED, NULL);
    g_ui.banner_dot = circle(b, 10, COL_BRASS);
    lv_obj_align(g_ui.banner_dot, LV_ALIGN_LEFT_MID, 22, 0);
    g_ui.banner_nfc = icon(b, &ui_icon_nfc_20, COL_BRASS);
    lv_obj_align(g_ui.banner_nfc, LV_ALIGN_LEFT_MID, 16, 0);
    g_ui.banner_lbl = label(b, F18, COL_CREAM, "");
    lv_obj_set_size(g_ui.banner_lbl, UI_W - 32 - 48 - 22, 22);
    lv_label_set_long_mode(g_ui.banner_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(g_ui.banner_lbl, LV_ALIGN_LEFT_MID, 48, 0);
}

static void banner_anim(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_delete(g_ui.banner, banner_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_ui.banner);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, banner_y_cb);
    if (done) {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
}

void uis_banner_show(ui_toast_kind_t kind, const char *text)
{
    uint32_t col = kind == UI_TOAST_ERROR ? COL_ERR : kind == UI_TOAST_WARN ? COL_WARN : COL_BRASS;
    lv_obj_set_style_border_color(g_ui.banner, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(g_ui.banner_dot, lv_color_hex(col), 0);
    show(g_ui.banner_dot, kind != UI_TOAST_NFC);
    show(g_ui.banner_nfc, kind == UI_TOAST_NFC);
    set_text_if(g_ui.banner_lbl, text);
    g_ui.banner_hide_at = ui_now() + (kind == UI_TOAST_WARN || kind == UI_TOAST_ERROR ? 7000 : 4000);
    bool was_on = g_ui.banner_on;
    g_ui.banner_on = true;
    show(g_ui.banner, true);
    if (!was_on) {
        banner_anim(-64, 10, NULL);
    }
}

void uis_banner_hide(void)
{
    if (!g_ui.banner_on) {
        return;
    }
    g_ui.banner_on = false;
    banner_anim(lv_obj_get_y(g_ui.banner), -64, banner_hidden_cb);
}

/* ======================================================================
 * settings
 * ====================================================================== */

static const char *const s_seg_text[3] = {"Pausieren", "Weiterspielen", "Titel zu Ende"};
static const char *const s_saver_text[3] = {"1 Minute", "5 Minuten", "15 Minuten"};
static const uint8_t s_saver_min[3] = {1, 5, 15};

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    ui_settings_close();
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(s);
    if (v != g_ui.model.brightness) {
        g_ui.model.brightness = (uint8_t)v;
        ui_send_cmd(UI_CMD_SET_BRIGHTNESS, v);
    }
}

static void seg_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx != g_ui.model.tag_remove_action) {
        g_ui.model.tag_remove_action = (uint8_t)idx; /* optimistic */
        uis_settings_sync();
        ui_send_cmd(UI_CMD_SET_TAG_REMOVE_ACTION, idx);
    }
}

static void saver_seg_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t min = s_saver_min[idx];
    if (min != g_ui.model.saver_minutes) {
        g_ui.model.saver_minutes = min; /* optimistic */
        g_ui.saver_timeout_ms = (uint32_t)min * 60000u;
        uis_settings_sync();
        ui_send_cmd(UI_CMD_SET_SAVER_MINUTES, min);
    }
}

/* three-way segmented control (brass pill on the selected one) */
static void seg_control(lv_obj_t *parent, lv_obj_t **segs, lv_obj_t **lbls, const char *const *texts,
                        lv_event_cb_t cb)
{
    lv_obj_t *seg = box(parent);
    lv_obj_set_pos(seg, 0, 30);
    lv_obj_set_size(seg, SET_W, 56);
    lv_obj_set_style_radius(seg, 28, 0);
    lv_obj_set_style_border_width(seg, 2, 0);
    lv_obj_set_style_border_color(seg, lv_color_hex(COL_BRASS_DK), 0);
    lv_obj_set_style_pad_all(seg, 2, 0);
    int32_t sw = (SET_W - 8) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *o = segs[i] = box(seg);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(o, i * sw, 0);
        lv_obj_set_size(o, i == 2 ? SET_W - 8 - 2 * sw : sw, 48);
        lv_obj_set_style_radius(o, 24, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(COL_PANEL_HI), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(o, 6);
        lbls[i] = label(o, F18, COL_CREAM, texts[i]);
        lv_obj_center(lbls[i]);
        lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void seg_sync(lv_obj_t **segs, lv_obj_t **lbls, int selected)
{
    for (int i = 0; i < 3; i++) {
        bool sel = selected == i;
        if (sel) {
            brass_fill(segs[i]);
        }
        else {
            lv_obj_set_style_bg_opa(segs[i], LV_OPA_TRANSP, 0);
        }
        lv_obj_set_style_text_color(lbls[i], lv_color_hex(sel ? COL_INK : COL_CREAM), 0);
    }
}

static void shuffle_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_ui.model.shuffle = on;
    update_player();
    ui_send_cmd(UI_CMD_SHUFFLE, on ? 1 : 0);
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;
    ui_settings_close();
    ui_send_cmd(UI_CMD_OPEN_SETUP, 0);
}

static void update_cb(lv_event_t *e)
{
    (void)e;
    if (g_ui.model.update_checking || g_ui.update_check_local) {
        return;
    }
    if (g_ui.model.update_available) {
        ui_send_cmd(UI_CMD_START_UPDATE, 0);
    }
    else {
        g_ui.update_check_local = true;
        uis_settings_sync();
        ui_send_cmd(UI_CMD_CHECK_UPDATE, 0);
    }
}

static void tag_cb(lv_event_t *e)
{
    (void)e;
    if (!g_ui.model.tag_write_supported) {
        uis_banner_show(UI_TOAST_INFO, "Kommt mit einem der nächsten Updates.");
        return;
    }
    if (!g_ui.model.context[0]) {
        uis_banner_show(UI_TOAST_WARN, "Erst eine Playlist abspielen, dann speichern.");
        return;
    }
    ui_settings_close();
    uis_banner_show(UI_TOAST_NFC, "Leeren Tag auf das Gerät legen …");
    ui_send_cmd(UI_CMD_WRITE_TAG, 0);
}

/* one block in the settings list: caps heading, content, hairline below */
static lv_obj_t *section(lv_obj_t *list, const char *title, bool last)
{
    lv_obj_t *s = box(list);
    lv_obj_set_size(s, SET_W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(s, 16, 0);
    if (!last) {
        lv_obj_set_style_border_side(s, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s, 1, 0);
        lv_obj_set_style_border_color(s, lv_color_hex(COL_LINE), 0);
    }
    lv_obj_t *c = caps(s, title);
    lv_obj_set_style_text_color(c, lv_color_hex(COL_BRASS), 0);
    return s;
}

/* value line + grey sub line on the left, button on the right */
static void row_with_button(lv_obj_t *s, lv_obj_t **val, lv_obj_t **sub, const char *btn, int32_t bw,
                            lv_event_cb_t cb, lv_obj_t **btn_out, lv_obj_t **btn_lbl)
{
    *val = label(s, F18, COL_CREAM, "");
    lv_obj_set_pos(*val, 0, 28);
    lv_obj_set_width(*val, SET_W - bw - 16);
    lv_label_set_long_mode(*val, LV_LABEL_LONG_MODE_DOTS);
    *sub = label(s, F14, COL_GREY, "");
    lv_obj_set_pos(*sub, 0, 54);
    lv_obj_set_width(*sub, SET_W - bw - 16);
    lv_label_set_long_mode(*sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *b = pill_button(s, btn, bw, cb, btn_lbl);
    lv_obj_set_pos(b, SET_W - bw, 26);
    if (btn_out) {
        *btn_out = b;
    }
}

static void style_switch(lv_obj_t *sw)
{
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 72, 40);
    lv_obj_set_ext_click_area(sw, 12);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_BRASS), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_GREY), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_CREAM), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -5, LV_PART_KNOB);
    lv_obj_set_style_anim_duration(sw, 150, 0);
}

static void build_settings(void)
{
    lv_obj_t *m = g_ui.settings = layer(g_ui.scr, true);

    /* header: back button + title, fixed */
    lv_obj_t *b = box(m);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(b, 6, 4);
    lv_obj_set_size(b, 156, 56);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL_HI), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_t *bi = icon(b, &ui_icon_back_24, COL_BRASS);
    lv_obj_align(bi, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *bl = label(b, F18, COL_CREAM, "Zurück");
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 42, 0);
    lv_obj_add_event_cb(b, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = caps(m, "Einstellungen");
    lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -SET_PAD, 25);
    lv_obj_t *hl = box(m);
    lv_obj_set_pos(hl, 0, SET_HEAD - 1);
    lv_obj_set_size(hl, UI_W, 1);
    lv_obj_set_style_bg_color(hl, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);

    /* scrolling list */
    lv_obj_t *l = g_ui.set_list = box(m);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(l, 0, SET_HEAD);
    lv_obj_set_size(l, UI_W, UI_H - SET_HEAD);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(l, SET_PAD, 0);
    lv_obj_set_style_pad_bottom(l, 8, 0);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(l, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(l, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(l, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(l, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(l, lv_color_hex(COL_BRASS), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(l, LV_OPA_70, LV_PART_SCROLLBAR);

    /* brightness */
    lv_obj_t *s = section(l, "Helligkeit", false);
    lv_obj_t *s1 = icon(s, &ui_icon_sun_20, COL_GREY);
    lv_obj_set_pos(s1, 0, 38);
    lv_obj_t *sl = g_ui.set_slider = lv_slider_create(s);
    lv_obj_remove_style_all(sl);
    lv_obj_set_size(sl, SET_W - 84, 6);
    lv_obj_set_pos(sl, 40, 45);
    lv_slider_set_range(sl, 5, 100);
    lv_obj_set_ext_click_area(sl, 26);
    lv_obj_set_style_radius(sl, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_BRASS), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_CREAM), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 12, LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, lv_color_hex(COL_BRASS), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *s2 = icon(s, &ui_icon_sun_28, COL_CREAM);
    lv_obj_set_pos(s2, SET_W - 28, 34);

    /* tag removed: segmented control */
    s = section(l, "Wenn der Tag abgenommen wird", false);
    seg_control(s, g_ui.set_seg, g_ui.set_seg_lbl, s_seg_text, seg_cb);

    /* screensaver delay */
    s = section(l, "Bildschirmschoner nach", false);
    seg_control(s, g_ui.set_saver_seg, g_ui.set_saver_lbl, s_saver_text, saver_seg_cb);

    /* shuffle */
    s = section(l, "Zufallswiedergabe", false);
    lv_obj_t *sl2 = label(s, F18, COL_CREAM, "Titel gemischt abspielen");
    lv_obj_set_pos(sl2, 0, 36);
    lv_obj_t *swi = g_ui.set_shuffle = lv_switch_create(s);
    style_switch(swi);
    lv_obj_set_pos(swi, SET_W - 72, 26);
    lv_obj_add_event_cb(swi, shuffle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Wi-Fi */
    s = section(l, "WLAN", false);
    row_with_button(s, &g_ui.set_wifi_val, &g_ui.set_wifi_sub, "Ändern", 124, wifi_cb, NULL, NULL);

    /* software */
    s = section(l, "Software", false);
    row_with_button(s, &g_ui.set_upd_val, &g_ui.set_upd_sub, "Prüfen", 148, update_cb, &g_ui.set_upd_btn,
                    &g_ui.set_upd_lbl);

    /* tags */
    s = section(l, "Tags", false);
    lv_obj_t *tv;
    row_with_button(s, &tv, &g_ui.set_tag_sub, "Speichern", 148, tag_cb, NULL, NULL);
    lv_label_set_text(tv, "Musik auf Tag speichern");

    /* device */
    s = section(l, "Gerät", true);
    static const char *const keys[3] = {"Name in Spotify", "IP-Adresse", "Spotify-Konto"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *k = label(s, F14, COL_GREY, keys[i]);
        lv_obj_set_pos(k, 0, 30 + i * 28 + 3);
        g_ui.set_dev_val[i] = label(s, F18, COL_CREAM, "");
        lv_obj_set_pos(g_ui.set_dev_val[i], 150, 30 + i * 28);
        lv_obj_set_width(g_ui.set_dev_val[i], SET_W - 150);
        lv_label_set_long_mode(g_ui.set_dev_val[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

void uis_settings_sync(void)
{
    const ui_model_t *m = &g_ui.model;
    char buf[96];
    if (!lv_obj_has_state(g_ui.set_slider, LV_STATE_PRESSED)) {
        int32_t b = m->brightness < 5 ? 5 : m->brightness > 100 ? 100 : m->brightness;
        lv_slider_set_value(g_ui.set_slider, b, LV_ANIM_OFF);
    }
    seg_sync(g_ui.set_seg, g_ui.set_seg_lbl, m->tag_remove_action);
    int saver_idx = m->saver_minutes <= 1 ? 0 : m->saver_minutes <= 5 ? 1 : 2;
    if (m->saver_minutes == 0) {
        saver_idx = g_ui.saver_timeout_ms <= 60000 ? 0 : g_ui.saver_timeout_ms <= 300000 ? 1 : 2;
    }
    seg_sync(g_ui.set_saver_seg, g_ui.set_saver_lbl, saver_idx);
    if (m->shuffle != lv_obj_has_state(g_ui.set_shuffle, LV_STATE_CHECKED)) {
        lv_obj_set_state(g_ui.set_shuffle, LV_STATE_CHECKED, m->shuffle);
    }

    if (!m->wifi_ok) {
        set_text_if(g_ui.set_wifi_val, m->wifi_ssid[0] ? m->wifi_ssid : "Nicht verbunden");
        set_text_if(g_ui.set_wifi_sub, "keine Verbindung");
    }
    else {
        set_text_if(g_ui.set_wifi_val, m->wifi_ssid[0] ? m->wifi_ssid : "Verbunden");
        if (m->rssi) {
            snprintf(buf, sizeof(buf), "Empfang %s", rssi_text(m->rssi));
        }
        else {
            buf[0] = '\0';
        }
        set_text_if(g_ui.set_wifi_sub, buf);
    }

    snprintf(buf, sizeof(buf), "Version %s", m->fw_version[0] ? m->fw_version : "–");
    set_text_if(g_ui.set_upd_val, buf);
    bool checking = m->update_checking || g_ui.update_check_local;
    if (checking) {
        set_text_if(g_ui.set_upd_sub, "Suche nach Updates …");
    }
    else if (m->update_available) {
        snprintf(buf, sizeof(buf), "Neu: Version %s", m->update_version[0] ? m->update_version : "?");
        set_text_if(g_ui.set_upd_sub, buf);
    }
    else {
        set_text_if(g_ui.set_upd_sub, m->update_text);
    }
    set_text_if(g_ui.set_upd_lbl, m->update_available && !checking ? "Installieren" : "Prüfen");
    lv_obj_set_style_opa(g_ui.set_upd_btn, checking ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (m->update_available && !checking) {
        brass_fill(g_ui.set_upd_btn);
        lv_obj_set_style_text_color(g_ui.set_upd_lbl, lv_color_hex(COL_INK), 0);
    }
    else {
        lv_obj_set_style_bg_opa(g_ui.set_upd_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(g_ui.set_upd_lbl, lv_color_hex(COL_CREAM), 0);
    }

    if (m->context[0]) {
        snprintf(buf, sizeof(buf), "Läuft gerade: %s", m->context);
    }
    else {
        snprintf(buf, sizeof(buf), "Erst Playlist oder Album abspielen");
    }
    set_text_if(g_ui.set_tag_sub, buf);

    set_text_if(g_ui.set_dev_val[0], dev_name());
    set_text_if(g_ui.set_dev_val[1], m->ip[0] ? m->ip : "–");
    set_text_if(g_ui.set_dev_val[2], m->linked ? "verbunden" : "nicht verbunden");
}

/* ======================================================================
 * screensaver
 * ====================================================================== */

static void saver_tap_cb(lv_event_t *e)
{
    (void)e;
    uis_saver_exit();
}

static void build_saver(void)
{
    /*
     *   14:37                      clock, 56 px
     *   Mittwoch, 14. Oktober      date
     *   ──                         short brass rule
     *   [cover] Abendrot           what is playing (hidden when idle)
     *           AnnenMayKantereit
     *
     * Nothing playing: only the clock (104 px, light) and the date, centred.
     * The card sits around the screen centre and shifts by up to 18/14 px
     * every 20 s; every pixel is dim (AMOLED burn-in).
     */
    lv_obj_t *s = g_ui.saver = layer(g_ui.scr, true);
    lv_obj_add_event_cb(s, saver_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *b = g_ui.saver_box = box(s);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 0, 0);

    g_ui.saver_clock = label(b, F56, SAVER_CLOCK, "");
    lv_obj_set_style_text_letter_space(g_ui.saver_clock, 1, 0);
    g_ui.saver_date = label(b, F14, SAVER_COL, "");
    lv_obj_set_style_text_letter_space(g_ui.saver_date, 1, 0);
    lv_obj_set_style_margin_top(g_ui.saver_date, 2, 0);
    lv_obj_set_style_margin_left(g_ui.saver_date, 3, 0);   /* line up with the digits */

    lv_obj_t *r = g_ui.saver_rule = box(b);
    lv_obj_set_size(r, 28, 2);
    lv_obj_set_style_bg_color(r, lv_color_hex(SAVER_BRASS), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(r, 14, 0);
    lv_obj_set_style_margin_bottom(r, 14, 0);

    lv_obj_t *row = g_ui.saver_row = box(b);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    /* cover thumbnail: the 180 px cover scaled down, dimmed */
    lv_obj_t *cv = box(row);
    lv_obj_set_size(cv, SAVER_THUMB, SAVER_THUMB);
    lv_obj_set_style_radius(cv, 6, 0);
    lv_obj_set_style_clip_corner(cv, true, 0);
    g_ui.saver_cover = lv_image_create(cv);
    lv_obj_remove_flag(g_ui.saver_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(g_ui.saver_cover, &ui_img_vinyl_180);
    lv_image_set_pivot(g_ui.saver_cover, 0, 0);
    lv_image_set_scale(g_ui.saver_cover, (uint32_t)(256 * SAVER_THUMB + 179) / 180);
    lv_image_set_antialias(g_ui.saver_cover, true);
    lv_obj_set_pos(g_ui.saver_cover, 0, 0);
    lv_obj_set_style_image_opa(g_ui.saver_cover, LV_OPA_50, 0);

    lv_obj_t *col = box(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    g_ui.saver_state = label(col, F14, SAVER_BRASS, "");
    lv_obj_set_style_text_letter_space(g_ui.saver_state, 2, 0);
    g_ui.saver_l1 = label(col, F18, SAVER_TEXT, "");
    lv_obj_set_style_max_width(g_ui.saver_l1, SAVER_TEXT_MAX, 0);
    lv_label_set_long_mode(g_ui.saver_l1, LV_LABEL_LONG_MODE_DOTS);
    g_ui.saver_l2 = label(col, F14, SAVER_COL, "");
    lv_obj_set_style_max_width(g_ui.saver_l2, SAVER_TEXT_MAX, 0);
    lv_label_set_long_mode(g_ui.saver_l2, LV_LABEL_LONG_MODE_DOTS);
}

void uis_saver_move(void)
{
    struct tm tm;
    const ui_model_t *m = &g_ui.model;
    bool music = g_ui.view == VIEW_PLAYER && m->title[0] &&
                 (m->state == UI_STATE_PLAYING || m->state == UI_STATE_PAUSED);
    bool clock = clock_now(&tm);

    show(g_ui.saver_clock, clock);
    show(g_ui.saver_date, clock);
    show(g_ui.saver_rule, clock && music);
    show(g_ui.saver_row, music || !clock);
    show(g_ui.saver_cover, music);
    show(lv_obj_get_parent(g_ui.saver_cover), music);
    show(g_ui.saver_state, music);
    show(g_ui.saver_l2, music && m->artist[0]);
    if (music) {
        caps_set(g_ui.saver_state, m->state == UI_STATE_PAUSED ? "Pausiert" : "Läuft gerade");
        set_text_if(g_ui.saver_l1, m->title);
        set_text_if(g_ui.saver_l2, m->artist);
    }
    else if (!clock) {
        set_text_if(g_ui.saver_l1, dev_name()); /* no time yet, nothing playing */
    }
    if (clock) {
        saver_set_date(&tm);
    }
    /* nothing playing: one big, light clock with the date, centred */
    bool big = clock && !music;
    lv_obj_set_style_text_font(g_ui.saver_clock, big ? F104 : F56, 0);
    lv_obj_set_style_text_letter_space(g_ui.saver_clock, big ? 0 : 1, 0);
    lv_obj_set_style_margin_left(g_ui.saver_date, big ? 0 : 3, 0);
    lv_obj_set_style_margin_top(g_ui.saver_date, big ? 6 : 2, 0);
    lv_obj_set_style_text_font(g_ui.saver_date, big ? F18 : F14, 0);
    lv_obj_set_flex_align(g_ui.saver_box, LV_FLEX_ALIGN_START,
                          big ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* around the centre, a little off each time */
    lv_obj_update_layout(g_ui.saver_box);
    int32_t w = lv_obj_get_width(g_ui.saver_box), h = lv_obj_get_height(g_ui.saver_box);
    int32_t x = (UI_W - w) / 2 + (int32_t)lv_rand(0, 2 * SAVER_DRIFT_X) - SAVER_DRIFT_X;
    int32_t y = (UI_H - h) / 2 + (int32_t)lv_rand(0, 2 * SAVER_DRIFT_Y) - SAVER_DRIFT_Y;
    x = LV_CLAMP(8, x, UI_W - 8 - w);
    y = LV_CLAMP(8, y, UI_H - 8 - h);
    lv_obj_set_pos(g_ui.saver_box, x, y);
    g_ui.saver_moved_at = ui_now();
}

void uis_saver_enter(void)
{
    if (g_ui.saver_on) {
        return;
    }
    g_ui.saver_on = true;
    uis_banner_hide();
    uis_saver_move();
    uis_refresh_layers();
}

void uis_saver_exit(void)
{
    if (!g_ui.saver_on) {
        return;
    }
    g_ui.saver_on = false;
    lv_display_trigger_activity(g_ui.disp);
    uis_refresh_layers();
}

/* ======================================================================
 * model -> screens
 * ====================================================================== */

ui_view_t uis_view_for(const ui_model_t *m)
{
    switch (m->state) {
    case UI_STATE_BOOT:
        return VIEW_SPLASH;
    case UI_STATE_SETUP_WIFI:
    case UI_STATE_SETUP_SPOTIFY:
        return VIEW_SETUP;
    case UI_STATE_UPDATE:
        return VIEW_OTA;
    case UI_STATE_ERROR:
        return VIEW_ERROR;
    case UI_STATE_PLAYING:
    case UI_STATE_PAUSED:
        return VIEW_PLAYER;
    case UI_STATE_CONNECTING_WIFI:
    case UI_STATE_WAIT_SPOTIFY:
    case UI_STATE_IDLE:
    default:
        return VIEW_HINT;
    }
}

void uis_apply_model(void)
{
    update_player();
    update_hint();
    if (g_ui.view == VIEW_SETUP) {
        update_setup();
    }
    update_ota();
    update_error();
    uis_settings_sync();
}

/* ======================================================================
 * build + visibility with a fade through black
 * ====================================================================== */

enum { FADE_IDLE, FADE_OUT, FADE_IN };
static int s_fade_phase;

#define FADE_OUT_MS 110
#define FADE_IN_MS  190

static uint32_t layers_key(void)
{
    return (uint32_t)g_ui.view | (g_ui.settings_open ? 0x10u : 0) | (g_ui.saver_on ? 0x20u : 0);
}

static void apply_layers(void)
{
    bool saver = g_ui.saver_on;
    bool settings = g_ui.settings_open && !saver;
    bool base = !saver && !settings;
    show(g_ui.splash, base && g_ui.view == VIEW_SPLASH);
    show(g_ui.setup, base && g_ui.view == VIEW_SETUP);
    show(g_ui.hint, base && g_ui.view == VIEW_HINT);
    show(g_ui.player, base && g_ui.view == VIEW_PLAYER);
    show(g_ui.ota, base && g_ui.view == VIEW_OTA);
    show(g_ui.error, base && g_ui.view == VIEW_ERROR);
    show(g_ui.settings, settings);
    show(g_ui.saver, saver);
    g_ui.shown_key = layers_key();
}

static void fader_exec(void *var, int32_t v)
{
    (void)var;
    lv_obj_set_style_bg_opa(g_ui.fader, (lv_opa_t)v, 0);
}

static void fade_start(int32_t from, int32_t to, uint32_t ms, lv_anim_completed_cb_t done)
{
    lv_anim_delete(g_ui.fader, fader_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_ui.fader);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, fader_exec);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void fade_in_done(lv_anim_t *a)
{
    (void)a;
    s_fade_phase = FADE_IDLE;
    show(g_ui.fader, false);
    if (layers_key() != g_ui.shown_key) {
        uis_refresh_layers();
    }
}

static void fade_out_done(lv_anim_t *a)
{
    (void)a;
    apply_layers();
    s_fade_phase = FADE_IN;
    fade_start(LV_OPA_COVER, LV_OPA_TRANSP, FADE_IN_MS, fade_in_done);
}

void uis_refresh_layers(void)
{
    if (g_ui.shown_key == UINT32_MAX) {
        apply_layers(); /* first time: no fade */
        return;
    }
    if (s_fade_phase == FADE_OUT || layers_key() == g_ui.shown_key) {
        return; /* fade-out completes with the latest state */
    }
    int32_t from = s_fade_phase == FADE_IN ? lv_obj_get_style_bg_opa(g_ui.fader, 0) : LV_OPA_TRANSP;
    s_fade_phase = FADE_OUT;
    lv_obj_set_style_bg_opa(g_ui.fader, (lv_opa_t)from, 0);
    show(g_ui.fader, true);
    fade_start(from, LV_OPA_COVER, FADE_OUT_MS * (uint32_t)(LV_OPA_COVER - from) / LV_OPA_COVER + 1,
               fade_out_done);
}

void uis_build(void)
{
    lv_anim_init(&s_marquee);
    lv_anim_set_delay(&s_marquee, 2500);
    lv_anim_set_repeat_count(&s_marquee, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&s_marquee, 3000);

    lv_obj_t *scr = g_ui.scr = lv_display_get_screen_active(g_ui.disp);
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_splash();
    build_setup();
    build_hint();
    build_player();
    build_ota();
    build_error();
    build_settings();
    build_saver();
    g_ui.fader = layer(scr, false);
    build_banner();
    g_ui.shown_key = UINT32_MAX;
}
