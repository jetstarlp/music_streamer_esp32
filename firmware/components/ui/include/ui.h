/*
 * ui.h - touch UI of the Music Streamer (LVGL 9.3, landscape 456 x 280).
 *
 * Usage:
 *   ui_set_command_handler(on_cmd, NULL);   // any time, any task
 *   display_lock(0); ui_init(disp); display_unlock();
 *   ui_model_publish(&model);               // any task, whenever something changes
 *
 * Threading: ui_init() must be called with the LVGL lock held. Every other
 * function may be called from any task without the LVGL lock: data is copied
 * under the UI's own mutex and applied by ui_tick(), which ui_init() runs
 * from an LVGL timer every 30 ms in the LVGL task.
 *
 * The UI uses only LVGL and libc/pthread (no ESP-IDF API), so the same code
 * runs in the PC simulator (simulator/).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build all screens on @p disp (456 x 280) and start the UI timer. Call once, LVGL lock held. */
void ui_init(lv_display_t *disp);

/**
 * Apply a newly published model, run timeouts, progress extrapolation and
 * the screensaver. Called automatically by the timer created in ui_init();
 * call it yourself only from the LVGL task (LVGL lock held).
 */
void ui_tick(void);

/**
 * Burn-in protection: after @p timeout_s without touch (default 300, 0 = off)
 * on the player, idle and waiting screens, only a dim, slowly wandering
 * title/clock remains on black. A touch only wakes the screen. The panel
 * brightness is not touched (dim it in the app with display_idle_ms() if
 * wanted).
 */
void ui_set_screensaver(uint32_t timeout_s);

#ifdef __cplusplus
}
#endif
