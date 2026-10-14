/*
 * Test-only network bring-up for Espressif's QEMU (OpenCores Ethernet MAC
 * emulation, "-nic user,model=open_eth"). Replaces WiFi when
 * CONFIG_MS_NET_QEMU_OPENETH=y (see sdkconfig.qemu and docs/BUILD.md).
 * Never enable this for a build that goes onto real hardware.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the emulated Ethernet interface and wait up to `timeout` for DHCP. */
esp_err_t net_qemu_start(TickType_t timeout);

#ifdef __cplusplus
}
#endif
