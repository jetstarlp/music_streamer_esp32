/*
 * Serial console of the bench firmware (USB-Serial-JTAG on the boards, UART
 * in the QEMU build). Commands: wifi, spotify, name, tone, nfc, status, reboot,
 * factory-reset; "help" lists them. See docs/BENCH_TEST.md.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register all commands and start the REPL task. Call once after settings_init(). */
esp_err_t console_start(void);

#ifdef __cplusplus
}
#endif
