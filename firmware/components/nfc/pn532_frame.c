/*
 * PN532 frame encoding/decoding, see pn532_frame.h. No ESP-IDF dependencies.
 */
#include "pn532_frame.h"

#include <string.h>

const uint8_t PN532_ACK_FRAME[6]  = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
const uint8_t PN532_NACK_FRAME[6] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
const uint8_t PN532_HSU_WAKEUP[16] = { 0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

size_t pn532_frame_build(uint8_t cmd, const uint8_t *params, size_t n, uint8_t *out, size_t cap)
{
    if (!out || n > PN532_MAX_PARAMS || (n && !params) || cap < PN532_FRAME_SIZE(n)) {
        return 0;
    }
    size_t len = n + 2;   /* TFI + command code + parameters */
    size_t i = 0;
    out[i++] = 0x00;      /* preamble */
    out[i++] = 0x00;      /* start code */
    out[i++] = 0xFF;
    out[i++] = (uint8_t)len;
    out[i++] = (uint8_t)(0x100 - len);
    uint8_t sum = PN532_TFI_HOST + cmd;
    out[i++] = PN532_TFI_HOST;
    out[i++] = cmd;
    for (size_t k = 0; k < n; k++) {
        out[i++] = params[k];
        sum = (uint8_t)(sum + params[k]);
    }
    out[i++] = (uint8_t)(0x100 - sum);
    out[i++] = 0x00;      /* postamble */
    return i;
}

/* ---- receiver ------------------------------------------------------------------ */

enum {
    S_START0 = 0,  /* waiting for 00                       */
    S_START1,      /* got 00, waiting for FF               */
    S_LEN,
    S_LCS,
    S_ELEN_H,      /* extended frame: LENM                 */
    S_ELEN_L,      /* extended frame: LENL                 */
    S_ELCS,
    S_DATA,
    S_DCS,
};

void pn532_rx_init(pn532_rx_t *rx, uint8_t *buf, size_t cap)
{
    memset(rx, 0, sizeof *rx);
    rx->buf = buf;
    rx->cap = cap;
}

void pn532_rx_reset(pn532_rx_t *rx)
{
    rx->state = S_START0;
    rx->len = 0;
    rx->pos = 0;
    rx->sum = 0;
}

/* Wrong length checksum: whatever came was no frame. The byte that broke it
 * may start the next one. */
static pn532_rx_event_t resync(pn532_rx_t *rx, uint8_t byte)
{
    pn532_rx_reset(rx);
    rx->state = byte == 0x00 ? S_START1 : S_START0;
    return PN532_RX_NONE;
}

pn532_rx_event_t pn532_rx_feed(pn532_rx_t *rx, uint8_t byte)
{
    switch (rx->state) {
    case S_START0:
        if (byte == 0x00) {
            rx->state = S_START1;
        }
        return PN532_RX_NONE;
    case S_START1:
        if (byte == 0xFF) {
            rx->state = S_LEN;
        } else if (byte != 0x00) {
            rx->state = S_START0;
        }
        return PN532_RX_NONE;
    case S_LEN:
        rx->len = byte;
        rx->state = S_LCS;
        return PN532_RX_NONE;
    case S_LCS:
        if (rx->len == 0x00 && byte == 0xFF) {
            pn532_rx_reset(rx);
            return PN532_RX_ACK;
        }
        if (rx->len == 0xFF && byte == 0x00) {
            pn532_rx_reset(rx);
            return PN532_RX_NACK;
        }
        if (rx->len == 0xFF && byte == 0xFF) {
            rx->state = S_ELEN_H;
            return PN532_RX_NONE;
        }
        if (rx->len == 0 || ((rx->len + byte) & 0xFF) != 0) {
            return resync(rx, byte);
        }
        rx->pos = 0;
        rx->sum = 0;
        rx->state = S_DATA;
        return PN532_RX_NONE;
    case S_ELEN_H:
        rx->lenm = byte;
        rx->state = S_ELEN_L;
        return PN532_RX_NONE;
    case S_ELEN_L:
        rx->len = (size_t)rx->lenm << 8 | byte;
        rx->state = S_ELCS;
        return PN532_RX_NONE;
    case S_ELCS:
        if (rx->len == 0 || ((rx->lenm + (rx->len & 0xFF) + byte) & 0xFF) != 0) {
            return resync(rx, byte);
        }
        rx->pos = 0;
        rx->sum = 0;
        rx->state = S_DATA;
        return PN532_RX_NONE;
    case S_DATA:
        if (rx->pos < rx->cap) {
            rx->buf[rx->pos] = byte;
        }
        rx->sum = (uint8_t)(rx->sum + byte);
        if (++rx->pos == rx->len) {
            rx->state = S_DCS;
        }
        return PN532_RX_NONE;
    case S_DCS: {
        bool ok = (uint8_t)(rx->sum + byte) == 0;
        size_t len = rx->len;
        pn532_rx_reset(rx);
        rx->len = len;            /* keep for the caller */
        if (len > rx->cap) {
            return PN532_RX_TOO_LONG;
        }
        if (!ok) {
            return PN532_RX_BAD_CHECKSUM;
        }
        return rx->buf[0] == PN532_TFI_ERROR ? PN532_RX_ERROR_FRAME : PN532_RX_FRAME;
    }
    default:
        pn532_rx_reset(rx);
        return PN532_RX_NONE;
    }
}

/* ---- answer parsers ---------------------------------------------------------------- */

bool pn532_parse_firmware(const uint8_t *p, size_t n, pn532_fw_t *out)
{
    if (!p || !out || n != 4) {
        return false;
    }
    out->ic = p[0];
    out->ver = p[1];
    out->rev = p[2];
    out->support = p[3];
    return true;
}

int pn532_parse_list_106a(const uint8_t *p, size_t n, pn532_target_a_t *out)
{
    if (!p || !out || n < 1) {
        return -1;
    }
    if (p[0] == 0) {
        return n == 1 ? 0 : -1;
    }
    /* Tg, SENS_RES (2), SEL_RES, NFCIDLength, NFCID1 [, ATS] */
    if (n < 6) {
        return -1;
    }
    size_t uid_len = p[5];
    if (uid_len != 4 && uid_len != 7 && uid_len != 10) {
        return -1;
    }
    size_t end = 6 + uid_len;
    if (n < end) {
        return -1;
    }
    if (p[4] & 0x20) {            /* ISO 14443-4 compliant: an ATS follows */
        if (n < end + 1 || p[end] == 0 || n < end + p[end]) {
            return -1;
        }
        end += p[end];
    }
    if (p[0] == 1 && n != end) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->tg = p[1];
    out->sens_res = (uint16_t)(p[2] << 8 | p[3]);
    out->sel_res = p[4];
    out->uid_len = (uint8_t)uid_len;
    memcpy(out->uid, p + 6, uid_len);
    return 1;
}

const char *pn532_status_str(uint8_t status)
{
    switch (PN532_STATUS_ERR(status)) {
    case 0x00: return "ok";
    case 0x01: return "timeout (no answer from the tag)";
    case 0x02: return "CRC error";
    case 0x03: return "parity error";
    case 0x04: return "wrong bit count during anticollision";
    case 0x05: return "framing error";
    case 0x06: return "abnormal bit collision";
    case 0x07: return "communication buffer too small";
    case 0x09: return "RF buffer overflow";
    case 0x0A: return "RF field not switched on in time";
    case 0x0B: return "RF protocol error";
    case 0x0D: return "temperature error";
    case 0x0E: return "internal buffer overflow";
    case 0x10: return "invalid parameter";
    case 0x12: return "DEP: unsupported command";
    case 0x13: return "DEP: wrong data format";
    case 0x14: return "authentication error / tag NAK";
    case 0x23: return "wrong UID check byte";
    case 0x25: return "invalid device state";
    case 0x26: return "operation not allowed";
    case 0x27: return "command not acceptable now";
    case 0x29: return "target released";
    case 0x2A: return "card ID mismatch";
    case 0x2B: return "card disappeared";
    case 0x2C: return "NFCID3 mismatch";
    case 0x2D: return "over-current";
    case 0x2E: return "NAD missing";
    default:   return "unknown PN532 error";
    }
}
