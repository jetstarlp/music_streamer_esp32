/*
 * ui.c - public API, hand-over between tasks and the LVGL task, gestures on
 * the player, timeouts and screensaver.
 *
 * ui_model_publish() copies the model (and a changed cover) into `s_pend`
 * under `s_mx` and sets a dirty flag. ui_tick() (LVGL timer, 30 ms) takes a
 * snapshot under the mutex and applies it to the LVGL objects. LVGL objects
 * are never touched outside the LVGL task.
 */
#include <string.h>

#include "ui.h"
#include "ui_internal.h"
#include "ui_port.h"

typedef struct {
    bool dirty;
    bool cover_dirty;
    ui_model_t model;
    /* cover: pixels owned by the UI, handed to the LVGL task with cover_dirty */
    uint16_t *cover;
    uint16_t cover_w, cover_h;
    const void *src_ptr; /* identity of the last copied cover */
    uint16_t src_w, src_h;
    uint32_t src_id;
    uint32_t saver_timeout_s;
    bool saver_dirty;
    void (*cmd_cb)(ui_cmd_t, int32_t, void *);
    void *cmd_ctx;
} pending_t;

static ui_mutex_t s_mx = UI_MUTEX_INIT;
static pending_t s_pend = {.saver_timeout_s = 300, .saver_dirty = true};
static bool s_inited;

ui_t g_ui;

#define LONG_PRESS_MS    700
#define MOVE_SLOP        14
#define SWIPE_MIN        60
#define SETTINGS_IDLE_MS 60000

/* ======================================================================
 * public API (any task)
 * ====================================================================== */

static void terminate(char *s, size_t cap)
{
    s[cap - 1] = '\0';
}

void ui_model_publish(const ui_model_t *m)
{
    if (!m) {
        return;
    }
    /* copy a changed cover outside the lock (64 kB memcpy) */
    uint16_t *copy = NULL;
    ui_mutex_lock(&s_mx);
    bool cover_changed = m->cover_rgb565 != s_pend.src_ptr || m->cover_w != s_pend.src_w ||
                         m->cover_h != s_pend.src_h || m->cover_id != s_pend.src_id;
    ui_mutex_unlock(&s_mx);
    if (cover_changed && m->cover_rgb565 && m->cover_w && m->cover_h) {
        size_t bytes = (size_t)m->cover_w * m->cover_h * 2;
        copy = ui_port_alloc_large(bytes);
        if (copy) {
            memcpy(copy, m->cover_rgb565, bytes);
        }
    }

    uint16_t *old = NULL;
    ui_mutex_lock(&s_mx);
    ui_model_t *p = &s_pend.model;
    *p = *m;
    terminate(p->title, sizeof(p->title));
    terminate(p->artist, sizeof(p->artist));
    terminate(p->context, sizeof(p->context));
    terminate(p->device_name, sizeof(p->device_name));
    terminate(p->wifi_ssid, sizeof(p->wifi_ssid));
    terminate(p->ip, sizeof(p->ip));
    terminate(p->setup_qr, sizeof(p->setup_qr));
    terminate(p->setup_ssid, sizeof(p->setup_ssid));
    terminate(p->setup_url, sizeof(p->setup_url));
    terminate(p->toast, sizeof(p->toast));
    terminate(p->update_version, sizeof(p->update_version));
    terminate(p->update_text, sizeof(p->update_text));
    terminate(p->error_title, sizeof(p->error_title));
    terminate(p->error_text, sizeof(p->error_text));
    terminate(p->fw_version, sizeof(p->fw_version));
    p->cover_rgb565 = NULL;
    s_pend.dirty = true;
    if (cover_changed) {
        old = s_pend.cover; /* not yet taken by the LVGL task */
        s_pend.cover = copy;
        s_pend.cover_w = copy ? m->cover_w : 0;
        s_pend.cover_h = copy ? m->cover_h : 0;
        s_pend.src_ptr = m->cover_rgb565;
        s_pend.src_w = m->cover_w;
        s_pend.src_h = m->cover_h;
        s_pend.src_id = m->cover_id;
        s_pend.cover_dirty = true;
    }
    ui_mutex_unlock(&s_mx);
    ui_port_free(old);
}

void ui_set_command_handler(void (*cb)(ui_cmd_t, int32_t, void *), void *ctx)
{
    ui_mutex_lock(&s_mx);
    s_pend.cmd_cb = cb;
    s_pend.cmd_ctx = ctx;
    ui_mutex_unlock(&s_mx);
}

void ui_set_screensaver(uint32_t timeout_s)
{
    ui_mutex_lock(&s_mx);
    s_pend.saver_timeout_s = timeout_s;
    s_pend.saver_dirty = true;
    ui_mutex_unlock(&s_mx);
}

/* ======================================================================
 * LVGL task
 * ====================================================================== */

uint32_t ui_now(void)
{
    return lv_tick_get();
}

void ui_send_cmd(ui_cmd_t cmd, int32_t arg)
{
    ui_mutex_lock(&s_mx);
    void (*cb)(ui_cmd_t, int32_t, void *) = s_pend.cmd_cb;
    void *ctx = s_pend.cmd_ctx;
    ui_mutex_unlock(&s_mx);
    UI_LOG("ui command %d (%d)", (int)cmd, (int)arg);
    if (cb) {
        cb(cmd, arg, ctx);
    }
}

static void wake(void)
{
    lv_display_trigger_activity(g_ui.disp);
    if (g_ui.saver_on) {
        uis_saver_exit();
    }
}

void ui_settings_open(void)
{
    if (g_ui.settings_open) {
        return;
    }
    g_ui.settings_open = true;
    g_ui.update_check_local = false;
    uis_settings_sync();
    lv_obj_scroll_to_y(g_ui.set_list, 0, LV_ANIM_OFF);
    uis_refresh_layers();
    ui_send_cmd(UI_CMD_OPEN_SETTINGS, 0);
}

void ui_settings_close(void)
{
    if (!g_ui.settings_open) {
        return;
    }
    g_ui.settings_open = false;
    uis_refresh_layers();
    ui_send_cmd(UI_CMD_CLOSE_SETTINGS, 0);
}

static void apply_pending(void)
{
    char old_title[sizeof(g_ui.model.title)];
    uint16_t *cover = NULL;
    uint16_t cw = 0, ch = 0;
    memcpy(old_title, g_ui.model.title, sizeof(old_title));
    ui_state_t old_state = g_ui.model.state;
    ui_mutex_lock(&s_mx);
    bool dirty = s_pend.dirty, cover_dirty = s_pend.cover_dirty;
    if (s_pend.saver_dirty) {
        g_ui.saver_timeout_ms = s_pend.saver_timeout_s * 1000u;
        s_pend.saver_dirty = false;
    }
    if (dirty) {
        g_ui.model = s_pend.model; /* replaces optimistic local changes too */
    }
    if (cover_dirty) {
        cover = s_pend.cover;
        cw = s_pend.cover_w;
        ch = s_pend.cover_h;
        s_pend.cover = NULL;
    }
    s_pend.dirty = s_pend.cover_dirty = false;
    ui_mutex_unlock(&s_mx);

    if (cover_dirty) {
        uis_set_cover(cover, cw, ch); /* takes ownership */
    }
    if (!dirty) {
        return;
    }
    const ui_model_t *m = &g_ui.model;
    bool new_title = strcmp(m->title, old_title) != 0;
    ui_view_t old_view = g_ui.view;
    g_ui.pos_ms_at_tick = m->position_ms;
    g_ui.pos_tick = ui_now();
    g_ui.update_check_local = false;
    g_ui.view = uis_view_for(m);

    if (g_ui.view != old_view) {
        /* setup, update and splash replace everything */
        if (g_ui.view == VIEW_SETUP || g_ui.view == VIEW_OTA || g_ui.view == VIEW_SPLASH) {
            ui_settings_close();
        }
        wake();
    }
    if (m->saver_minutes) {
        g_ui.saver_timeout_ms = (uint32_t)m->saver_minutes * 60000u;
    }
    uis_apply_model();
    uis_update_progress(true);
    if (m->toast_id != g_ui.toast_id) {
        g_ui.toast_id = m->toast_id;
        if (m->toast[0]) {
            wake();
            uis_banner_show(m->toast_kind, m->toast);
        }
    }
    if ((new_title || m->state != old_state) && g_ui.saver_on) {
        uis_saver_move();
    }
    uis_refresh_layers();
}

static bool saver_allowed(void)
{
    return (g_ui.view == VIEW_PLAYER || g_ui.view == VIEW_HINT) && !g_ui.settings_open &&
           g_ui.model.state != UI_STATE_CONNECTING_WIFI;
}

static void check_timeouts(void)
{
    uint32_t now = ui_now();
    uint32_t idle = lv_display_get_inactive_time(g_ui.disp);
    if (g_ui.banner_on && (int32_t)(now - g_ui.banner_hide_at) >= 0) {
        uis_banner_hide();
    }
    if (g_ui.settings_open && idle > SETTINGS_IDLE_MS) {
        ui_settings_close();
    }
    if (!g_ui.saver_on && saver_allowed() && g_ui.saver_timeout_ms && idle >= g_ui.saver_timeout_ms) {
        uis_saver_enter();
    }
    else if (g_ui.saver_on && !saver_allowed()) {
        uis_saver_exit();
    }
    else if (g_ui.saver_on && now - g_ui.saver_moved_at >= 20000) {
        uis_saver_move();
    }
}

void ui_tick(void)
{
    if (!s_inited) {
        return;
    }
    apply_pending();
    check_timeouts();
    uis_update_progress(false);
    uis_update_clock();
    uis_update_fades();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    ui_tick();
}

/* ---- gestures on the player -------------------------------------------- */

void ui_player_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_ui.g_start = p;
        g_ui.g_start_tick = ui_now();
        g_ui.g_moved = false;
        g_ui.g_long = false;
        g_ui.g_active = true;
        g_ui.g_zone = uis_zone_at(p);
        uis_zone_feedback(g_ui.g_zone, true);
        return;
    }
    if (!g_ui.g_active) {
        return;
    }
    int32_t dx = p.x - g_ui.g_start.x;
    int32_t dy = p.y - g_ui.g_start.y;
    if (code == LV_EVENT_PRESSING) {
        if (!g_ui.g_moved && (LV_ABS(dx) > MOVE_SLOP || LV_ABS(dy) > MOVE_SLOP)) {
            g_ui.g_moved = true;
            uis_zone_feedback(g_ui.g_zone, false);
        }
        if (g_ui.g_moved && LV_ABS(dx) > LV_ABS(dy)) {
            uis_drag(dx);
        }
        if (!g_ui.g_moved && !g_ui.g_long && ui_now() - g_ui.g_start_tick >= LONG_PRESS_MS) {
            /* long press anywhere: settings (the gear is the obvious way) */
            g_ui.g_long = true;
            uis_zone_feedback(g_ui.g_zone, false);
            ui_settings_open();
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        g_ui.g_active = false;
        uis_zone_feedback(g_ui.g_zone, false);
        if (g_ui.g_long || code == LV_EVENT_PRESS_LOST) {
            uis_swipe_done(0);
            return;
        }
        if (g_ui.g_moved) {
            if (LV_ABS(dx) >= SWIPE_MIN && LV_ABS(dx) * 2 > LV_ABS(dy) * 3) {
                uis_swipe_done(dx < 0 ? -1 : 1);
                ui_send_cmd(dx < 0 ? UI_CMD_NEXT : UI_CMD_PREV, 0);
            }
            else {
                uis_swipe_done(0);
            }
            return;
        }
        switch (g_ui.g_zone) {
        case ZONE_COVER:
        case ZONE_PLAY: {
            /* optimistic: show the new state at once, the app confirms */
            bool playing = g_ui.model.state == UI_STATE_PLAYING;
            g_ui.pos_ms_at_tick += playing ? ui_now() - g_ui.pos_tick : 0;
            g_ui.pos_tick = ui_now();
            g_ui.model.state = playing ? UI_STATE_PAUSED : UI_STATE_PLAYING;
            g_ui.model.playing = !playing;
            uis_apply_model();
            ui_send_cmd(UI_CMD_TOGGLE, 0);
            break;
        }
        case ZONE_PREV:
            uis_swipe_done(1);
            ui_send_cmd(UI_CMD_PREV, 0);
            break;
        case ZONE_NEXT:
            uis_swipe_done(-1);
            ui_send_cmd(UI_CMD_NEXT, 0);
            break;
        default:
            break;
        }
    }
}

void ui_init(lv_display_t *disp)
{
    if (s_inited || !disp) {
        return;
    }
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.disp = disp;
    g_ui.view = VIEW_SPLASH;
    g_ui.saver_timeout_ms = 300000;
    g_ui.last_pos_s = g_ui.last_dur_s = g_ui.last_clock_min = -1;
    g_ui.model.wifi_ok = true;
    g_ui.model.brightness = 80;

    uis_build();
    uis_apply_model();
    uis_refresh_layers();
    s_inited = true;
    lv_timer_create(tick_cb, 30, NULL);
}
