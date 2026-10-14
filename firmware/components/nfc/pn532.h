/*
 * PN532 in HSU mode (UART, 115200 8N1): low-level command layer.
 * Single instance, not thread-safe: nfc.c serialises all calls with its mutex.
 *
 * Return codes of the command functions:
 *   ESP_OK                    answer received (for tag commands: status 0)
 *   ESP_ERR_TIMEOUT           no ACK or no answer from the PN532
 *   ESP_ERR_INVALID_RESPONSE  garbled answer, error frame, unexpected IC
 *   ESP_FAIL                  the PN532 answered, but the tag operation failed
 *                             (status byte != 0, or wrong answer length)
 * pn532_last_error() describes the last failure.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pn532_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      uart_num;
    int      tx_gpio;       /* MCU TX -> PN532 "SCL/RX"            */
    int      rx_gpio;       /* MCU RX <- PN532 "SDA/TX"            */
    int      rstpd_n_gpio;  /* -1 if not wired                      */
    uint32_t baud;
} pn532_cfg_t;

esp_err_t pn532_open(const pn532_cfg_t *cfg);
/* RF field off is the caller's job; this holds the PN532 in reset (power down). */
void      pn532_close(void);

/* Hardware reset (if wired), HSU wake-up, SAMConfiguration (normal mode),
 * GetFirmwareVersion, passive activation retries, RF field off. */
esp_err_t pn532_init_reader(pn532_fw_t *fw);

esp_err_t pn532_rf_field(bool on);

/* InListPassiveTarget, 106 kbps type A, MaxTg 1. *found = false (and ESP_OK)
 * when no tag answered. */
esp_err_t pn532_list_106a(pn532_target_a_t *tg, bool *found);

/* InDataExchange with target 1 (NTAG READ/WRITE). */
esp_err_t pn532_data_exchange(const uint8_t *tx, size_t n, uint8_t *rx, size_t cap, size_t *rx_len);

/* InCommunicateThru (raw command with CRC, e.g. NTAG GET_VERSION). */
esp_err_t pn532_communicate_thru(const uint8_t *tx, size_t n, uint8_t *rx, size_t cap, size_t *rx_len);

const char *pn532_last_error(void);

#ifdef __cplusplus
}
#endif
