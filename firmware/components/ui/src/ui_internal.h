/*
 * ui_internal.h - shared between ui.c (hand-over, state, gestures) and
 * ui_screens.c (building and updating the LVGL objects). Everything here is
 * used in the LVGL task only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_icons.h"

/* ---- look: "Richtung C" ------------------------------------------------ */
#define UI_W 456
#define UI_H 280

#define COL_BG       0x000000
#define COL_BRASS    0xC9A45C
#define COL_BRASS_HI 0xDDBD78
#define COL_BRASS_LO 0xB08A45
#define COL_BRASS_DK 0x7A6233 /* groove on the brass bar */
#define COL_CREAM    0xF2E8D5
#define COL_GREY     0xB5AB98
#define COL_DIM      0x808080
#define COL_LINE     0x2B2721
#define COL_PANEL    0x13110E
#define COL_PANEL_HI 0x2A251D
#define COL_ERR      0xE0705A
#define COL_WARN     0xE3B04B
#define COL_INK      0x15120D /* "black" text on brass */

#define BAR_H   36
#define BAR_Y   (UI_H - BAR_H)
#define COVER   UI_COVER_SIZE
#define COVER_X 20
#define COVER_Y 30
#define COL_X   224
#define COL_W   (UI_W - COL_X - 16)

/* settings button, top right column */
#define GEAR_X (UI_W - 64)
#define GEAR_W 64

LV_FONT_DECLARE(ui_font_barlow_14);
LV_FONT_DECLARE(ui_font_barlow_18);
LV_FONT_DECLARE(ui_font_barlow_24);
LV_FONT_DECLARE(ui_font_barlow_32);
LV_FONT_DECLARE(ui_font_barlow_56);
LV_FONT_DECLARE(ui_font_barlow_104);

/* full screens; several ui_state_t share one */
typedef enum { VIEW_SPLASH, VIEW_SETUP, VIEW_HINT, VIEW_PLAYER, VIEW_OTA, VIEW_ERROR } ui_view_t;

/* touch zones on the player */
typedef enum { ZONE_NONE, ZONE_COVER, ZONE_PREV, ZONE_PLAY, ZONE_NEXT } ui_zone_t;

typedef struct {
    lv_display_t *disp;
    lv_obj_t *scr;

    /* player */
    lv_obj_t *player;
    lv_obj_t *cover_img, *cover_shade;
    lv_obj_t *col, *lbl_context, *lbl_title, *lbl_artist;
    lv_obj_t *btn_prev, *btn_play, *btn_next, *ico_play;
    lv_obj_t *status_box, *st_nfc, *st_wifi, *st_shuffle;
    lv_obj_t *bar_fill, *lbl_pos, *lbl_dur;

    /* hint: connecting / waiting for Spotify / idle */
    lv_obj_t *hint, *hint_spinner, *hint_caps, *hint_title, *hint_clock_big, *hint_body;
    lv_obj_t *hint_tag_row, *hint_tag_lbl, *hint_name, *hint_clock;

    /* other full screens */
    lv_obj_t *setup, *qr, *setup_caps, *setup_step[3], *setup_note, *setup_cancel;
    lv_obj_t *ota, *ota_pct, *ota_fill, *ota_note;
    lv_obj_t *error, *err_title, *err_text;
    lv_obj_t *splash;

    /* settings overlay */
    lv_obj_t *settings, *set_list, *set_slider, *set_seg[3], *set_seg_lbl[3], *set_saver_seg[3], *set_saver_lbl[3], *set_shuffle;
    lv_obj_t *set_wifi_val, *set_wifi_sub, *set_upd_val, *set_upd_sub, *set_upd_btn, *set_upd_lbl;
    lv_obj_t *set_tag_sub, *set_dev_val[3];

    /* toast, screensaver, fader */
    lv_obj_t *banner, *banner_dot, *banner_nfc, *banner_lbl;
    lv_obj_t *saver, *saver_box, *saver_l1, *saver_l2, *saver_clock;
    lv_obj_t *saver_date, *saver_rule, *saver_row, *saver_cover, *saver_state;
    lv_obj_t *fader;

    /* state (LVGL task only) */
    ui_model_t model;        /* cover fields unused here, see cover_* */
    uint32_t pos_ms_at_tick, pos_tick;
    ui_view_t view;          /* wanted */
    uint32_t shown_key;      /* view/overlay combination currently on screen */
    bool settings_open;
    bool update_check_local; /* "Prüfen" tapped, waiting for the app */
    uint32_t toast_id;
    uint32_t saver_timeout_ms;
    bool saver_on;
    uint32_t saver_moved_at;
    uint32_t banner_hide_at;
    bool banner_on;
    int last_pos_s, last_dur_s, last_clock_min;
    char qr_shown[160];

    /* cover */
    uint16_t *cover_buf;
    lv_image_dsc_t cover_dsc[2];
    int cover_dsc_idx;

    /* gesture tracking on the player */
    lv_point_t g_start;
    uint32_t g_start_tick;
    bool g_moved, g_long, g_active;
    ui_zone_t g_zone;
} ui_t;

extern ui_t g_ui;

/* ui.c */
void ui_send_cmd(ui_cmd_t cmd, int32_t arg);
uint32_t ui_now(void);
void ui_settings_open(void);
void ui_settings_close(void);
void ui_player_event_cb(lv_event_t *e);

/* ui_screens.c */
void uis_build(void);
void uis_apply_model(void);
void uis_update_progress(bool force);
void uis_update_clock(void);
void uis_update_fades(void);
void uis_set_cover(uint16_t *px, uint16_t w, uint16_t h);
void uis_refresh_layers(void);
void uis_banner_show(ui_toast_kind_t kind, const char *text);
void uis_banner_hide(void);
void uis_settings_sync(void);
void uis_saver_enter(void);
void uis_saver_exit(void);
void uis_saver_move(void);
void uis_zone_feedback(ui_zone_t z, bool pressed);
ui_zone_t uis_zone_at(lv_point_t p);
void uis_drag(int32_t dx);
void uis_swipe_done(int dir); /* -1 next (swiped left), +1 prev, 0 cancelled */
ui_view_t uis_view_for(const ui_model_t *m);

/* helpers (ui_screens.c) */
void ui_utf8_upper(const char *src, char *dst, size_t cap);
void ui_fmt_time(char *buf, size_t cap, uint32_t ms);
