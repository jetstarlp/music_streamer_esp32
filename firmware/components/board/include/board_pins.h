/*
 * Pin map: Waveshare ESP32-S3-Touch-AMOLED-1.64 (Rev 1.1, ESP32-S3R8) plus the
 * external modules of the Music Streamer (PCM5102A DAC, PN532 NFC reader).
 *
 * Source: board schematic Rev 1.1 and the pin plan of the project.
 * Plain GPIO numbers are used on purpose: the header has no dependencies and
 * can be included from C, C++ and host-side tests alike.
 *
 * Rule: every GPIO used anywhere in the firmware is named here and nowhere else.
 * Only the external DAC pins are exercised by this skeleton; the on-board
 * display/touch/IMU/SD assignments are listed for the drivers still to come
 * and have NOT been tested on hardware yet.
 */
#pragma once

/* ---- External: PCM5102A DAC (GY-PCM5102), 3-wire I2S, no MCLK ------------
 * Module solder bridges: SCK -> GND (the DAC derives its system clock from BCK
 * with its internal PLL), FLT = L, DEMP = L, FMT = L (I2S/Philips), XSMT driven
 * by the MCU. */
#define BOARD_I2S_BCK_GPIO          5   /* DAC BCK                               */
#define BOARD_I2S_WS_GPIO           6   /* DAC LCK (LRCK / word select)          */
#define BOARD_I2S_DOUT_GPIO         7   /* DAC DIN                               */
#define BOARD_DAC_XSMT_GPIO         8   /* DAC soft mute, high = un-muted. An
                                           external 10k pull-down keeps the DAC
                                           muted while the MCU is in reset.      */

/* ---- Board selection (menuconfig -> "Music Streamer board") ---------------
 * Host-side tests have no sdkconfig.h; they get the Waveshare map. */
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_MS_BOARD_XIAO_ESP32S3_PLUS)
/* Seeed XIAO ESP32-S3 Plus (ESP32-S3R8, bench tests). Labels on the board:
 * I2S BCK=D4, LCK=D5, DIN=D8, XSMT=D9 (same GPIOs as on the Waveshare),
 * NFC TX=D10, RX=D0, RSTPD_N=D1. GPIO43/44 (D6/D7) are avoided for the NFC
 * UART because the ROM bootloader prints on GPIO43. */
#define BOARD_NAME                  "XIAO ESP32-S3 Plus"
#define BOARD_HAS_DISPLAY           0
#define BOARD_NFC_UART_TX_GPIO      9   /* D10: MCU TX -> PN532 "SCL/RX"         */
#define BOARD_NFC_UART_RX_GPIO      1   /* D0:  MCU RX <- PN532 "SDA/TX"         */
#define BOARD_NFC_RSTPD_N_GPIO      2   /* D1:  PN532 RSTPD_N                     */
#define BOARD_STATUS_LED_GPIO       21  /* user LED, active low                   */
#define BOARD_SPARE_GPIO            3   /* D2 (strapping pin, input use only)     */
#else
#define BOARD_NAME                  "Waveshare ESP32-S3-Touch-AMOLED-1.64"
#define BOARD_HAS_DISPLAY           1
/* ---- External: PN532 NFC reader, HSU (UART) mode, 115200 baud ------------ */
#define BOARD_NFC_UART_TX_GPIO      15  /* MCU TX -> PN532 "SCL/RX"              */
#define BOARD_NFC_UART_RX_GPIO      16  /* MCU RX <- PN532 "SDA/TX" (10k to 3V3) */
#define BOARD_NFC_RSTPD_N_GPIO      2   /* PN532 RSTPD_N: high = run, low = reset */
#define BOARD_STATUS_LED_GPIO       (-1) /* none                                 */
/* ---- Spare header pin ---------------------------------------------------- */
#define BOARD_SPARE_GPIO            1
#endif

/* ---- On-board: AMOLED (QSPI) ---------------------------------------------- */
#define BOARD_LCD_QSPI_CLK_GPIO     10
#define BOARD_LCD_QSPI_D0_GPIO      11
#define BOARD_LCD_QSPI_D1_GPIO      12
#define BOARD_LCD_QSPI_D2_GPIO      13
#define BOARD_LCD_QSPI_D3_GPIO      14
#define BOARD_LCD_CS_GPIO           46  /* strapping pin                         */
#define BOARD_LCD_TE_GPIO           45  /* tearing effect; strapping pin         */
#define BOARD_LCD_RESET_GPIO        21  /* shared reset of display and touch     */

/* ---- On-board: I2C bus (touch FT3168/FT5x06 @0x38, IMU QMI8658 @0x6B) ----- */
#define BOARD_I2C_SDA_GPIO          47
#define BOARD_I2C_SCL_GPIO          48
#define BOARD_TP_INT_GPIO           18  /* touch interrupt (also on the header)  */
#define BOARD_IMU_INT1_GPIO         9
#define BOARD_IMU_INT2_GPIO         17  /* (also on the header)                  */

/* ---- On-board: misc -------------------------------------------------------- */
#define BOARD_BOOT_BUTTON_GPIO      0   /* BOOT button; strapping pin            */
#define BOARD_BAT_ADC_GPIO          4   /* battery voltage divider (ADC1_CH3)    */
#define BOARD_USB_DM_GPIO           19  /* native USB = USB-Serial-JTAG console  */
#define BOARD_USB_DP_GPIO           20

/* micro-SD slot: GPIO 38..41 are wired to the card socket. The signal mapping
 * has to be taken from the schematic by whoever writes the SD driver. */
#define BOARD_SD_GPIO_38            38
#define BOARD_SD_GPIO_39            39
#define BOARD_SD_GPIO_40            40
#define BOARD_SD_GPIO_41            41

/* Never use:
 *   GPIO 26..32  SPI flash
 *   GPIO 33..37  octal PSRAM of the ESP32-S3R8
 * Conditionally usable: GPIO 3 (strapping), GPIO 43/44 (UART0). */
