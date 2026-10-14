/*
 * display - AMOLED (CO5300, QSPI), touch (FT3168, I2C) and LVGL 9 for the
 * Waveshare ESP32-S3-Touch-AMOLED-1.64 Rev 1.1.
 *
 * The panel is 280 x 456 portrait; the UI runs in landscape 456 x 280 by
 * default (menuconfig -> "Music Streamer display" -> rotation). LVGL 9 rotates
 * touch coordinates together with the display.
 *
 * Threading: LVGL runs in its own task (esp_lvgl_port). Every LVGL call from
 * another task must be wrapped in display_lock()/display_unlock().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise QSPI panel, I2C bus, touch and LVGL. Returns the LVGL display
 * (already rotated as configured) or NULL on error. Call once.
 */
lv_display_t *display_init(void);

/** Panel brightness 0..100 % (CO5300 command 0x51). */
esp_err_t display_set_brightness(uint8_t percent);

/** Last value set with display_set_brightness() (or the Kconfig default). */
uint8_t display_get_brightness(void);

/**
 * Switch the picture off/on. Uses the panel's display-off command, NOT deep
 * sleep: the touch controller shares the reset line with the panel and would
 * lose its state. Touch keeps working while the picture is off.
 */
esp_err_t display_set_on(bool on);

/** Take/give the LVGL lock. timeout_ms 0 = wait forever. */
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

/** Milliseconds since the screen was last touched (for dimming). */
uint32_t display_idle_ms(void);

/**
 * The shared I2C bus (SDA 47 / SCL 48: touch 0x38, IMU 0x6B). Valid after
 * display_init(); other drivers add their devices to it.
 */
i2c_master_bus_handle_t display_i2c_bus(void);

#ifdef __cplusplus
}
#endif
