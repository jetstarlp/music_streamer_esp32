/*
 * PN532 HSU driver: UART transport, ACK/NACK handling, command layer.
 * Frame encoding/decoding lives in pn532_frame.c (host-tested).
 */
#include "pn532.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pn532";

#define ACK_TIMEOUT_MS        40    /* the ACK comes within ~1 ms of the command   */
#define CMD_TIMEOUT_MS        100   /* simple commands                             */
#define LIST_TIMEOUT_MS       300   /* InListPassiveTarget with limited retries    */
#define TAG_TIMEOUT_MS        150   /* InDataExchange: PN532 waits up to 51 ms     */
#define PASSIVE_RETRIES       0x08  /* MxRtyPassiveActivation (0xFF = forever)     */
#define RESET_LOW_MS          20
#define BOOT_WAIT_MS          100
#define UART_RX_BUF           512   /* driver ring buffer (> hardware FIFO)        */

static struct {
    bool              open;
    pn532_cfg_t       cfg;
    pn532_rx_t        rx;
    uint8_t           rxbuf[128];   /* TFI + PD of the frame being received        */
    uint8_t           txbuf[PN532_FRAME_SIZE(24)];
    uint8_t           chunk[64];    /* bytes read from the UART, not yet parsed     */
    size_t            chunk_pos, chunk_len;
    char              err[64];
} s;

static void delay_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

#define SET_ERR(...) snprintf(s.err, sizeof s.err, __VA_ARGS__)

const char *pn532_last_error(void)
{
    return s.err;
}

/* ---- transport ----------------------------------------------------------------- */

static void rx_discard(void)
{
    uart_flush_input(s.cfg.uart_num);
    s.chunk_pos = s.chunk_len = 0;
    pn532_rx_reset(&s.rx);
}

static esp_err_t uart_send(const uint8_t *data, size_t len)
{
    int n = uart_write_bytes(s.cfg.uart_num, data, len);
    return n == (int)len ? ESP_OK : ESP_FAIL;
}

/* Feeds received bytes to the frame parser until it reports something or the
 * deadline passes (PN532_RX_NONE). Blocks in the UART driver, never spins. */
static pn532_rx_event_t wait_event(int64_t deadline_us)
{
    for (;;) {
        while (s.chunk_pos < s.chunk_len) {
            pn532_rx_event_t ev = pn532_rx_feed(&s.rx, s.chunk[s.chunk_pos++]);
            if (ev != PN532_RX_NONE) {
                return ev;
            }
        }
        int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0) {
            return PN532_RX_NONE;
        }
        size_t avail = 0;
        uart_get_buffered_data_len(s.cfg.uart_num, &avail);
        int got;
        if (avail) {
            got = uart_read_bytes(s.cfg.uart_num, s.chunk, avail < sizeof s.chunk ? avail : sizeof s.chunk, 0);
        } else {
            TickType_t ticks = pdMS_TO_TICKS((uint32_t)((left_us + 999) / 1000));
            got = uart_read_bytes(s.cfg.uart_num, s.chunk, 1, ticks ? ticks : 1);
        }
        s.chunk_pos = 0;
        s.chunk_len = got > 0 ? (size_t)got : 0;
    }
}

/* Sends a command and returns the answer parameters (after TFI and answer code). */
static esp_err_t transceive(uint8_t cmd, const uint8_t *params, size_t n, uint8_t *resp, size_t cap,
                            size_t *resp_len, uint32_t timeout_ms)
{
    if (!s.open) {
        SET_ERR("reader not open");
        return ESP_ERR_INVALID_STATE;
    }
    size_t flen = pn532_frame_build(cmd, params, n, s.txbuf, sizeof s.txbuf);
    if (!flen) {
        SET_ERR("command 0x%02X too long", cmd);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Command -> ACK; a NACK or a lost ACK gives one more try. */
    bool acked = false;
    for (int attempt = 0; attempt < 2 && !acked; attempt++) {
        rx_discard();
        if (uart_send(s.txbuf, flen) != ESP_OK) {
            SET_ERR("UART write failed");
            return ESP_ERR_TIMEOUT;
        }
        int64_t deadline = esp_timer_get_time() + ACK_TIMEOUT_MS * 1000;
        for (;;) {
            pn532_rx_event_t ev = wait_event(deadline);
            if (ev == PN532_RX_ACK) {
                acked = true;
                break;
            }
            if (ev == PN532_RX_NONE || ev == PN532_RX_NACK || ev == PN532_RX_ERROR_FRAME) {
                break;     /* timeout, or the PN532 did not understand: resend */
            }
            /* a stale answer frame: keep waiting for the ACK */
        }
    }
    if (!acked) {
        SET_ERR("no ACK for command 0x%02X", cmd);
        return ESP_ERR_TIMEOUT;
    }

    /* ACK -> answer. A data checksum error is answered with NACK: the PN532
     * then sends the same answer again. */
    int nacks = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        pn532_rx_event_t ev = wait_event(deadline);
        switch (ev) {
        case PN532_RX_NONE:
            uart_send(PN532_ACK_FRAME, sizeof PN532_ACK_FRAME);   /* abort the command */
            delay_ms(2);
            rx_discard();
            SET_ERR("no answer to command 0x%02X", cmd);
            return ESP_ERR_TIMEOUT;
        case PN532_RX_FRAME: {
            size_t len = s.rx.len;
            if (len < 2 || s.rxbuf[0] != PN532_TFI_PN532 || s.rxbuf[1] != (uint8_t)(cmd + 1)) {
                continue;   /* not ours */
            }
            size_t plen = len - 2;
            if (plen > cap) {
                SET_ERR("answer to 0x%02X too long (%u bytes)", cmd, (unsigned)plen);
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (plen) {
                memcpy(resp, s.rxbuf + 2, plen);
            }
            *resp_len = plen;
            return ESP_OK;
        }
        case PN532_RX_BAD_CHECKSUM:
            if (nacks++ < 2) {
                uart_send(PN532_NACK_FRAME, sizeof PN532_NACK_FRAME);
                continue;
            }
            SET_ERR("answer to 0x%02X: checksum errors", cmd);
            return ESP_ERR_INVALID_RESPONSE;
        case PN532_RX_ERROR_FRAME:
            SET_ERR("PN532 error frame for command 0x%02X", cmd);
            return ESP_ERR_INVALID_RESPONSE;
        case PN532_RX_TOO_LONG:
            SET_ERR("answer to 0x%02X too long", cmd);
            return ESP_ERR_INVALID_RESPONSE;
        case PN532_RX_ACK:
        case PN532_RX_NACK:
            continue;
        }
    }
}

/* ---- setup ------------------------------------------------------------------------ */

esp_err_t pn532_open(const pn532_cfg_t *cfg)
{
    if (s.open) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s, 0, sizeof s);
    s.cfg = *cfg;
    pn532_rx_init(&s.rx, s.rxbuf, sizeof s.rxbuf);

    if (cfg->rstpd_n_gpio >= 0) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->rstpd_n_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_level((gpio_num_t)cfg->rstpd_n_gpio, 1);
    }

    const uart_config_t uc = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(cfg->uart_num, UART_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(cfg->uart_num, &uc);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* Idle-high line even without the module's own pull-up. */
        gpio_set_pull_mode((gpio_num_t)cfg->rx_gpio, GPIO_PULLUP_ONLY);
    }
    if (err != ESP_OK) {
        uart_driver_delete(cfg->uart_num);
        return err;
    }
    s.open = true;
    return ESP_OK;
}

void pn532_close(void)
{
    if (!s.open) {
        return;
    }
    uart_driver_delete(s.cfg.uart_num);
    if (s.cfg.rstpd_n_gpio >= 0) {
        gpio_set_level((gpio_num_t)s.cfg.rstpd_n_gpio, 0);   /* reset = power down */
    }
    s.open = false;
}

static esp_err_t rf_config(const uint8_t *p, size_t n)
{
    uint8_t resp[4];
    size_t rl;
    return transceive(PN532_CMD_RF_CONFIGURATION, p, n, resp, sizeof resp, &rl, CMD_TIMEOUT_MS);
}

esp_err_t pn532_init_reader(pn532_fw_t *fw)
{
    if (!s.open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s.cfg.rstpd_n_gpio >= 0) {
        gpio_set_level((gpio_num_t)s.cfg.rstpd_n_gpio, 0);
        delay_ms(RESET_LOW_MS);
        gpio_set_level((gpio_num_t)s.cfg.rstpd_n_gpio, 1);
        delay_ms(BOOT_WAIT_MS);
    }

    /* Wake-up preamble, then SAMConfiguration must be the first command:
     * normal mode (no SAM), timeout 1 s (unused), no IRQ pin. */
    static const uint8_t sam[] = { 0x01, 0x14, 0x00 };
    uint8_t resp[16];
    size_t rl = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 2 && err != ESP_OK; attempt++) {
        rx_discard();
        uart_send(PN532_HSU_WAKEUP, sizeof PN532_HSU_WAKEUP);
        delay_ms(5);
        err = transceive(PN532_CMD_SAM_CONFIGURATION, sam, sizeof sam, resp, sizeof resp, &rl, CMD_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = transceive(PN532_CMD_GET_FIRMWARE_VERSION, NULL, 0, resp, sizeof resp, &rl, CMD_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    pn532_fw_t v;
    if (!pn532_parse_firmware(resp, rl, &v)) {
        SET_ERR("malformed firmware version answer");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (v.ic != 0x32) {
        SET_ERR("unexpected IC 0x%02X (no PN532?)", v.ic);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (fw) {
        *fw = v;
    }

    /* MaxRetries: MxRtyATR default, MxRtyPSL default, few passive activations
     * so that InListPassiveTarget returns quickly when no tag is there. */
    static const uint8_t retries[] = { 0x05, 0xFF, 0x01, PASSIVE_RETRIES };
    err = rf_config(retries, sizeof retries);
    if (err == ESP_OK) {
        err = pn532_rf_field(false);
    }
    ESP_LOGD(TAG, "init done: %s", esp_err_to_name(err));
    return err;
}

esp_err_t pn532_rf_field(bool on)
{
    const uint8_t p[] = { 0x01, on ? 0x01 : 0x00 };   /* item 1: RF field, AutoRFCA off */
    return rf_config(p, sizeof p);
}

/* ---- tag commands ------------------------------------------------------------------- */

esp_err_t pn532_list_106a(pn532_target_a_t *tg, bool *found)
{
    static const uint8_t p[] = { 0x01, 0x00 };   /* MaxTg 1, BrTy 106 kbps type A */
    uint8_t resp[64];
    size_t rl = 0;
    *found = false;
    esp_err_t err = transceive(PN532_CMD_IN_LIST_PASSIVE_TARGET, p, sizeof p, resp, sizeof resp, &rl,
                               LIST_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    int n = pn532_parse_list_106a(resp, rl, tg);
    if (n < 0) {
        SET_ERR("malformed InListPassiveTarget answer (%u bytes)", (unsigned)rl);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *found = n > 0;
    return ESP_OK;
}

/* Status byte + data. */
static esp_err_t tag_command(uint8_t cmd, const uint8_t *p, size_t n, uint8_t *rx, size_t cap,
                             size_t *rx_len)
{
    uint8_t resp[40];
    size_t rl = 0;
    esp_err_t err = transceive(cmd, p, n, resp, sizeof resp, &rl, TAG_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (rl < 1) {
        SET_ERR("empty answer to 0x%02X", cmd);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (PN532_STATUS_ERR(resp[0]) != 0) {
        SET_ERR("tag: %s (0x%02X)", pn532_status_str(resp[0]), resp[0]);
        return ESP_FAIL;
    }
    if (rl - 1 > cap) {
        SET_ERR("tag answer too long (%u bytes)", (unsigned)(rl - 1));
        return ESP_FAIL;
    }
    memcpy(rx, resp + 1, rl - 1);
    *rx_len = rl - 1;
    return ESP_OK;
}

esp_err_t pn532_data_exchange(const uint8_t *tx, size_t n, uint8_t *rx, size_t cap, size_t *rx_len)
{
    uint8_t p[1 + 16];
    if (n > sizeof p - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    p[0] = 0x01;   /* target 1 */
    memcpy(p + 1, tx, n);
    return tag_command(PN532_CMD_IN_DATA_EXCHANGE, p, n + 1, rx, cap, rx_len);
}

esp_err_t pn532_communicate_thru(const uint8_t *tx, size_t n, uint8_t *rx, size_t cap, size_t *rx_len)
{
    return tag_command(PN532_CMD_IN_COMMUNICATE_THRU, tx, n, rx, cap, rx_len);
}
