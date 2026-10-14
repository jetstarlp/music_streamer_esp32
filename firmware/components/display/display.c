/*
 * display - CO5300 AMOLED over QSPI + FT3168 touch + LVGL 9 via esp_lvgl_port.
 *
 * Panel setup follows Waveshare's official Rev 1.1 example (06_LVGL_Test):
 * same init commands, 40 MHz QSPI, x offset 20, even-aligned flush windows.
 * The panel driver is espressif/esp_lcd_co5300 instead of Waveshare's
 * patched esp_lcd_sh8601.
 *
 * Touch is read directly (two I2C register reads, exactly like Waveshare's
 * touch_bsp.c) with the new i2c_master driver; LVGL 9 rotates the native
 * portrait coordinates together with the display.
 */
#include "display.h"

#include <string.h>

#include "board_pins.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "display";

#define LCD_HOST            SPI2_HOST
#define LCD_H_RES           280         /* native (portrait) */
#define LCD_V_RES           456
#define LCD_X_GAP           20          /* the 280 visible columns start at column 20 */
#define TOUCH_I2C_ADDR      0x38
#define TOUCH_I2C_HZ        400000
#define I2C_TIMEOUT_MS      20

#if BOARD_HAS_DISPLAY == 0
#error "The display component needs a board with display (Waveshare AMOLED profile)"
#endif

/* Waveshare's init sequence for this panel (Rev 1.1 example). */
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 80},   /* sleep out */
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},    /* tearing effect line on */
    {0x53, (uint8_t[]){0x20}, 1, 1},    /* brightness control on */
    {0x63, (uint8_t[]){0xFF}, 1, 1},
    {0x51, (uint8_t[]){0x00}, 1, 1},    /* brightness 0 while the RAM is undefined */
    {0x29, (uint8_t[]){0x00}, 0, 10},   /* display on */
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_touch;
static volatile int64_t s_last_touch_us;
static uint8_t s_brightness = CONFIG_MS_DISPLAY_DEFAULT_BRIGHTNESS;
static uint32_t s_touch_errors;

/* CO5300: windows must start on an even and end on an odd pixel. Because both
 * native dimensions are even, rounding in logical (rotated) coordinates gives
 * a valid native window for every rotation. */
static void rounder_cb(lv_area_t *area)
{
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

/* ---- touch ------------------------------------------------------------------- */

static esp_err_t touch_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_touch, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t points = 0;
    uint8_t xy[4];
    if (touch_read_reg(0x02, &points, 1) == ESP_OK && (points & 0x0F) > 0 &&
        touch_read_reg(0x03, xy, sizeof(xy)) == ESP_OK) {
        int32_t x = ((xy[0] & 0x0F) << 8) | xy[1];
        int32_t y = ((xy[2] & 0x0F) << 8) | xy[3];
        data->point.x = x < LCD_H_RES ? x : LCD_H_RES - 1;
        data->point.y = y < LCD_V_RES ? y : LCD_V_RES - 1;
        data->state = LV_INDEV_STATE_PRESSED;
        s_last_touch_us = esp_timer_get_time();
        return;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

static esp_err_t touch_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                     /* any free controller */
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* the board also has 4.7 k pull-ups */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG, "I2C bus");
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_touch), TAG, "touch device");

    /* Normal operating mode (register 0x00 = 0), as in Waveshare's example. */
    const uint8_t normal_mode[2] = {0x00, 0x00};
    esp_err_t err = i2c_master_transmit(s_touch, normal_mode, sizeof(normal_mode), 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not answering at 0x%02x (%s) - touch disabled",
                 TOUCH_I2C_ADDR, esp_err_to_name(err));
        s_touch_errors++;
    }
    return err;
}

/* ---- panel ---------------------------------------------------------------------- */

static esp_err_t panel_init(void)
{
    const size_t buf_px = LCD_H_RES * CONFIG_MS_DISPLAY_BUFFER_LINES;
    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(
        BOARD_LCD_QSPI_CLK_GPIO, BOARD_LCD_QSPI_D0_GPIO, BOARD_LCD_QSPI_D1_GPIO,
        BOARD_LCD_QSPI_D2_GPIO, BOARD_LCD_QSPI_D3_GPIO, buf_px * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(BOARD_LCD_CS_GPIO, NULL, NULL);
    io_cfg.pclk_hz = CONFIG_MS_DISPLAY_QSPI_MHZ * 1000 * 1000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "panel IO");

    const co5300_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = BOARD_LCD_RESET_GPIO,   /* also resets the touch controller */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &dev_cfg, &s_panel), TAG, "panel driver");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0), TAG, "panel gap");
    return ESP_OK;
}

static lv_display_rotation_t rotation_from_kconfig(void)
{
    switch (CONFIG_MS_DISPLAY_ROTATION) {
    case 90:  return LV_DISPLAY_ROTATION_90;
    case 180: return LV_DISPLAY_ROTATION_180;
    case 270: return LV_DISPLAY_ROTATION_270;
    default:  return LV_DISPLAY_ROTATION_0;
    }
}

lv_display_t *display_init(void)
{
    if (s_disp) {
        return s_disp;
    }
    if (panel_init() != ESP_OK) {
        return NULL;
    }
    /* The panel reset also reset the touch controller: initialise it now. */
    bool touch_ok = touch_init() == ESP_OK;

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;          /* below the audio feeder and WiFi */
    port_cfg.task_affinity = 0;          /* core 0; Spotify runs on core 1 */
    port_cfg.task_stack = 8192;
    /* Stack in PSRAM: the LVGL task only renders and talks to the panel and
     * the touch controller; it never writes flash (settings are saved by the
     * app controller task), so it does not need an internal stack. Saves 8 kB
     * of internal RAM. */
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * CONFIG_MS_DISPLAY_BUFFER_LINES,
        /* One render buffer: with sw_rotate the port allocates a second
         * (rotation) buffer of the same size in DMA memory anyway, and a
         * third one saves ~11 kB of internal RAM for little speed. */
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rounder_cb = rounder_cb,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
            .swap_bytes = true,          /* panel expects big-endian RGB565 */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }

    lvgl_port_lock(0);
    lv_display_set_rotation(s_disp, rotation_from_kconfig());
    if (touch_ok) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, s_disp);
    }
    lvgl_port_unlock();

    /* Draw the first frame before the panel lights up: its RAM content is
     * undefined after reset (the init sequence leaves brightness at 0). */
    lvgl_port_lock(0);
    lv_refr_now(s_disp);
    lvgl_port_unlock();
    s_last_touch_us = esp_timer_get_time();
    display_set_brightness(s_brightness);
    ESP_LOGI(TAG, "ready: %dx%d, rotation %d, QSPI %d MHz, touch %s",
             (int)lv_display_get_horizontal_resolution(s_disp),
             (int)lv_display_get_vertical_resolution(s_disp), CONFIG_MS_DISPLAY_ROTATION,
             CONFIG_MS_DISPLAY_QSPI_MHZ, touch_ok ? "on" : "OFF");
    return s_disp;
}

esp_err_t display_set_brightness(uint8_t percent)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }
    esp_err_t err = esp_lcd_panel_co5300_set_brightness(s_panel, percent);
    if (err == ESP_OK) {
        s_brightness = percent;
    }
    return err;
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

esp_err_t display_set_on(bool on)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_disp_on_off(s_panel, on);
}

bool display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}

uint32_t display_idle_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_last_touch_us) / 1000);
}

i2c_master_bus_handle_t display_i2c_bus(void)
{
    return s_i2c;
}
