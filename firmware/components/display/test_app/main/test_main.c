/*
 * Display bring-up test for the Waveshare ESP32-S3-Touch-AMOLED-1.64.
 *
 * What to look for (docs/DISPLAY.md):
 * - a white 1-pixel frame exactly on all four screen edges (offset / rounding),
 * - corner squares: red top left, green top right, blue bottom left, white
 *   bottom right (orientation / mirroring / colour order),
 * - "456 x 280" upright and readable (rotation),
 * - a cross that follows the finger, coordinates in the log and on screen,
 * - the slider changes the brightness, the frame rate shows bottom right.
 */
#include <stdio.h>

#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "display_test";

static lv_obj_t *s_cross_h;
static lv_obj_t *s_cross_v;
static lv_obj_t *s_touch_label;

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void label(lv_obj_t *parent, const char *text, lv_align_t align, int32_t dx, int32_t dy, uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_align(l, align, dx, dy);
}

static void on_press(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(s_cross_h, p.x - 15, p.y);
    lv_obj_set_pos(s_cross_v, p.x, p.y - 15);
    lv_label_set_text_fmt(s_touch_label, "touch %d, %d", (int)p.x, (int)p.y);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "touch at %d,%d", (int)p.x, (int)p.y);
    }
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(slider);
    display_set_brightness((uint8_t)v);
}

static void build_screen(lv_display_t *disp)
{
    const int32_t w = lv_display_get_horizontal_resolution(disp);
    const int32_t h = lv_display_get_vertical_resolution(disp);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 1-pixel frame on the outermost pixels */
    box(scr, 0, 0, w, 1, 0xFFFFFF);
    box(scr, 0, h - 1, w, 1, 0xFFFFFF);
    box(scr, 0, 0, 1, h, 0xFFFFFF);
    box(scr, w - 1, 0, 1, h, 0xFFFFFF);

    /* corner squares, 24 px, 2 px inside the frame */
    box(scr, 3, 3, 24, 24, 0xFF0000);
    box(scr, w - 27, 3, 24, 24, 0x00FF00);
    box(scr, 3, h - 27, 24, 24, 0x0000FF);
    box(scr, w - 27, h - 27, 24, 24, 0xFFFFFF);

    /* colour bars: if red/blue are swapped, the byte or colour order is wrong */
    static const uint32_t bars[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xC9A45C, 0xF2E8D5, 0x808080};
    const int32_t bw = (w - 80) / 6;
    for (int i = 0; i < 6; i++) {
        box(scr, 40 + i * bw, 40, bw, 30, bars[i]);
    }

    char text[64];
    snprintf(text, sizeof(text), "%d x %d  (rotation %d)", (int)w, (int)h, CONFIG_MS_DISPLAY_ROTATION);
    label(scr, text, LV_ALIGN_CENTER, 0, -30, 0xF2E8D5);
    label(scr, "R G B  brass cream grey", LV_ALIGN_TOP_MID, 0, 74, 0xB5AB98);

    s_touch_label = lv_label_create(scr);
    lv_label_set_text(s_touch_label, "touch the screen");
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0xC9A45C), 0);
    lv_obj_align(s_touch_label, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_width(slider, w - 120);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -45);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, display_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    label(scr, "brightness", LV_ALIGN_BOTTOM_MID, 0, -62, 0xB5AB98);

    s_cross_h = box(scr, -40, -40, 31, 1, 0xFFFF00);
    s_cross_v = box(scr, -40, -40, 1, 31, 0xFFFF00);
    lv_obj_add_event_cb(scr, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_press, LV_EVENT_PRESSING, NULL);
}

void app_main(void)
{
    lv_display_t *disp = display_init();
    if (!disp) {
        ESP_LOGE(TAG, "display_init failed");
        return;
    }
    display_lock(0);
    build_screen(disp);
    display_unlock();
    ESP_LOGI(TAG, "test pattern shown");
}
