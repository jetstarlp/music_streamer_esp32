/*
 * PN532 host interface frames (user manual UM0701-02, section 6.2), pure C99
 * without ESP-IDF dependencies; tested on the PC in tests/host/nfc.
 *
 *   normal information frame   00 00 FF LEN LCS TFI PD0 .. PDn DCS 00
 *   extended information frame 00 00 FF FF FF LENM LENL LCS TFI PD0 .. PDn DCS 00
 *   ACK                        00 00 FF 00 FF 00
 *   NACK                       00 00 FF FF 00 00
 *   error frame                00 00 FF 01 FF 7F 81 00   (syntax error in the host frame)
 *
 * LEN counts TFI + PD bytes, LEN + LCS = 0 (mod 256), TFI + PD.. + DCS = 0.
 * TFI is D4 from host to PN532 and D5 from PN532 to host; PD0 is the command
 * code (host) or command code + 1 (answer).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_TFI_HOST          0xD4
#define PN532_TFI_PN532         0xD5
#define PN532_TFI_ERROR         0x7F

#define PN532_CMD_GET_FIRMWARE_VERSION  0x02
#define PN532_CMD_SAM_CONFIGURATION     0x14
#define PN532_CMD_RF_CONFIGURATION      0x32
#define PN532_CMD_IN_DATA_EXCHANGE      0x40
#define PN532_CMD_IN_COMMUNICATE_THRU   0x42
#define PN532_CMD_IN_LIST_PASSIVE_TARGET 0x4A

/* Largest command parameter block that fits a normal frame (TFI + code + params <= 255). */
#define PN532_MAX_PARAMS        253
/* Bytes needed by pn532_frame_build() for n parameter bytes. */
#define PN532_FRAME_SIZE(n)     ((size_t)(n) + 9)

extern const uint8_t PN532_ACK_FRAME[6];
extern const uint8_t PN532_NACK_FRAME[6];
/* HSU wake-up: 55 55 followed by zeros (UM 7.2.11 / application note: the
 * PN532 needs a long preamble to leave the low-power state after reset). */
extern const uint8_t PN532_HSU_WAKEUP[16];

/* Builds a normal information frame host -> PN532 with TFI D4, command code
 * cmd and n parameter bytes. Returns the frame length, 0 if n > PN532_MAX_PARAMS,
 * params == NULL with n > 0, or cap < PN532_FRAME_SIZE(n). */
size_t pn532_frame_build(uint8_t cmd, const uint8_t *params, size_t n, uint8_t *out, size_t cap);

/* ---- Streaming receiver ------------------------------------------------------
 * Bytes from the UART are fed one at a time. Garbage between frames and
 * frames with a wrong length checksum are skipped silently (the receiver
 * resynchronises on the next 00 FF start code). */
typedef enum {
    PN532_RX_NONE = 0,     /* need more bytes                                     */
    PN532_RX_ACK,          /* ACK frame                                           */
    PN532_RX_NACK,         /* NACK frame                                          */
    PN532_RX_FRAME,        /* information frame: buf[0] = TFI, buf[1..] = PD, len */
    PN532_RX_ERROR_FRAME,  /* PN532 error frame (TFI 7F): it rejected our frame   */
    PN532_RX_BAD_CHECKSUM, /* information frame with a wrong data checksum (DCS)  */
    PN532_RX_TOO_LONG,     /* information frame longer than the buffer (skipped)  */
} pn532_rx_event_t;

typedef struct {
    uint8_t *buf;          /* receives TFI + PD of the current frame              */
    size_t   cap;
    size_t   len;          /* LEN of the frame (valid after PN532_RX_FRAME)       */
    size_t   pos;
    uint8_t  state;
    uint8_t  sum;
    uint8_t  lenm;
} pn532_rx_t;

void pn532_rx_init(pn532_rx_t *rx, uint8_t *buf, size_t cap);
void pn532_rx_reset(pn532_rx_t *rx);
pn532_rx_event_t pn532_rx_feed(pn532_rx_t *rx, uint8_t byte);

/* ---- Answer parsers ------------------------------------------------------------
 * p / n: the answer parameters, i.e. the frame data after TFI and answer code. */

typedef struct {
    uint8_t ic;            /* 0x32 for the PN532                                  */
    uint8_t ver, rev;      /* firmware 1.6 on all known modules                   */
    uint8_t support;       /* bit 0 ISO/IEC 14443 A, bit 1 B, bit 2 ISO 18092     */
} pn532_fw_t;

/* GetFirmwareVersion: exactly 4 bytes. */
bool pn532_parse_firmware(const uint8_t *p, size_t n, pn532_fw_t *out);

#define PN532_UID_MAX 10
typedef struct {
    uint8_t  tg;           /* logical target number (1)                           */
    uint16_t sens_res;     /* ATQA, as sent (first byte = high byte here)         */
    uint8_t  sel_res;      /* SAK: 0x00 = NFC Forum Type 2 (NTAG, Ultralight)     */
    uint8_t  uid_len;      /* 4, 7 or 10                                          */
    uint8_t  uid[PN532_UID_MAX];
} pn532_target_a_t;

/* InListPassiveTarget, 106 kbps type A. Returns the number of targets (0 or
 * 1; the first one is stored in *out) or -1 for a malformed answer. An ATS
 * (ISO 14443-4 cards, SAK bit 5) is checked for length and skipped. */
int pn532_parse_list_106a(const uint8_t *p, size_t n, pn532_target_a_t *out);

/* Text for the error code in the status byte of InDataExchange /
 * InCommunicateThru (bits 0..5; bit 6 = more information, bit 7 = NAD). */
const char *pn532_status_str(uint8_t status);
#define PN532_STATUS_ERR(s)   ((uint8_t)((s) & 0x3F))

#ifdef __cplusplus
}
#endif
