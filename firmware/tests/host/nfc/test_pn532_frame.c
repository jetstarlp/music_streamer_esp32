/*
 * Host tests for components/nfc/pn532_frame.c (pure C, no ESP-IDF).
 * Byte sequences from the PN532 user manual (UM0701-02) and hand-built answers.
 * Run: firmware/tools/host_test_nfc.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pn532_frame.h"

static int s_checks, s_fail;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        s_checks++;                                                                  \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            s_fail++;                                                                \
        }                                                                            \
    } while (0)

static size_t hexb(const char *hex, uint8_t *out)
{
    size_t n = 0;
    while (*hex) {
        if (*hex == ' ' || *hex == '\n') {
            hex++;
            continue;
        }
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1) {
            abort();
        }
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}

static bool built_equals(uint8_t cmd, const char *params_hex, const char *want_hex)
{
    uint8_t params[300], want[320], got[320];
    size_t np = hexb(params_hex, params);
    size_t nw = hexb(want_hex, want);
    size_t ng = pn532_frame_build(cmd, params, np, got, sizeof got);
    return ng == nw && memcmp(got, want, nw) == 0;
}

/* Feeds a byte stream; records the events and, for frames, a copy of TFI+PD. */
typedef struct {
    pn532_rx_event_t ev[16];
    uint8_t          data[16][300];
    size_t           len[16];
    int              n;
} feed_log_t;

static void feed(pn532_rx_t *rx, const uint8_t *b, size_t n, feed_log_t *log)
{
    for (size_t i = 0; i < n; i++) {
        pn532_rx_event_t ev = pn532_rx_feed(rx, b[i]);
        if (ev != PN532_RX_NONE && log->n < 16) {
            log->ev[log->n] = ev;
            log->len[log->n] = rx->len;
            if (ev == PN532_RX_FRAME || ev == PN532_RX_ERROR_FRAME) {
                memcpy(log->data[log->n], rx->buf, rx->len);
            }
            log->n++;
        }
    }
}

static void feed_hex(pn532_rx_t *rx, const char *hex, feed_log_t *log)
{
    uint8_t b[600];
    size_t n = hexb(hex, b);
    feed(rx, b, n, log);
}

static void test_build(void)
{
    /* UM0701-02 examples */
    CHECK(built_equals(PN532_CMD_GET_FIRMWARE_VERSION, "", "00 00 FF 02 FE D4 02 2A 00"));
    CHECK(built_equals(PN532_CMD_SAM_CONFIGURATION, "01 14 01", "00 00 FF 05 FB D4 14 01 14 01 02 00"));
    CHECK(built_equals(PN532_CMD_IN_LIST_PASSIVE_TARGET, "01 00", "00 00 FF 04 FC D4 4A 01 00 E1 00"));
    /* InDataExchange Tg 1, NTAG READ page 4 */
    CHECK(built_equals(PN532_CMD_IN_DATA_EXCHANGE, "01 30 04", "00 00 FF 05 FB D4 40 01 30 04 B7 00"));
    /* RFConfiguration MaxRetries FF 01 08 */
    CHECK(built_equals(PN532_CMD_RF_CONFIGURATION, "05 FF 01 08", "00 00 FF 06 FA D4 32 05 FF 01 08 ED 00"));
    /* RF field off */
    CHECK(built_equals(PN532_CMD_RF_CONFIGURATION, "01 00", "00 00 FF 04 FC D4 32 01 00 F9 00"));

    /* limits */
    uint8_t params[300] = { 0 }, out[300];
    CHECK(pn532_frame_build(0x40, params, PN532_MAX_PARAMS, out, sizeof out) == PN532_FRAME_SIZE(253));
    CHECK(out[3] == 0xFF && out[4] == 0x01);          /* LEN 255, LCS 01 */
    CHECK(pn532_frame_build(0x40, params, PN532_MAX_PARAMS + 1, out, sizeof out) == 0);
    CHECK(pn532_frame_build(0x02, NULL, 0, out, 8) == 0);   /* needs 9 */
    CHECK(pn532_frame_build(0x02, NULL, 0, out, 9) == 9);
    CHECK(pn532_frame_build(0x40, NULL, 2, out, sizeof out) == 0);
    CHECK(pn532_frame_build(0x02, NULL, 0, NULL, 9) == 0);

    /* constants */
    uint8_t b[16];
    hexb("00 00 FF 00 FF 00", b);
    CHECK(memcmp(PN532_ACK_FRAME, b, 6) == 0);
    hexb("00 00 FF FF 00 00", b);
    CHECK(memcmp(PN532_NACK_FRAME, b, 6) == 0);
    CHECK(PN532_HSU_WAKEUP[0] == 0x55 && PN532_HSU_WAKEUP[1] == 0x55 && PN532_HSU_WAKEUP[15] == 0x00);
}

static void test_receive_basic(void)
{
    uint8_t buf[64];
    pn532_rx_t rx;
    pn532_rx_init(&rx, buf, sizeof buf);
    feed_log_t log = { .n = 0 };

    /* ACK, then the GetFirmwareVersion answer from the manual (IC 32, v1.6, 07) */
    feed_hex(&rx, "00 00 FF 00 FF 00  00 00 FF 06 FA D5 03 32 01 06 07 E8 00", &log);
    CHECK(log.n == 2);
    CHECK(log.ev[0] == PN532_RX_ACK);
    CHECK(log.ev[1] == PN532_RX_FRAME);
    CHECK(log.len[1] == 6);
    CHECK(memcmp(log.data[1], "\xD5\x03\x32\x01\x06\x07", 6) == 0);
    pn532_fw_t fw;
    CHECK(pn532_parse_firmware(log.data[1] + 2, log.len[1] - 2, &fw));
    CHECK(fw.ic == 0x32 && fw.ver == 1 && fw.rev == 6 && fw.support == 7);
    CHECK(!pn532_parse_firmware(log.data[1] + 2, 3, &fw));
    CHECK(!pn532_parse_firmware(NULL, 4, &fw));

    /* NACK; preamble omitted (PN532 may skip it); several leading zeros */
    log.n = 0;
    feed_hex(&rx, "00 FF FF 00 00  00 00 00 00 FF 02 FE D5 15 16 00", &log);
    CHECK(log.n == 2 && log.ev[0] == PN532_RX_NACK && log.ev[1] == PN532_RX_FRAME);
    CHECK(log.len[1] == 2 && log.data[1][0] == 0xD5 && log.data[1][1] == 0x15);

    /* error frame (syntax error) */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 01 FF 7F 81 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_ERROR_FRAME);

    /* extended frame carrying a short answer */
    log.n = 0;
    feed_hex(&rx, "00 00 FF FF FF 00 03 FD D5 4B 00 E0 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_FRAME && log.len[0] == 3);
    CHECK(memcmp(log.data[0], "\xD5\x4B\x00", 3) == 0);
}

static void test_receive_errors(void)
{
    uint8_t buf[64];
    pn532_rx_t rx;
    pn532_rx_init(&rx, buf, sizeof buf);
    feed_log_t log = { .n = 0 };

    /* garbage (incl. 00 and FF bytes) before the frame */
    feed_hex(&rx, "FF 55 00 12 00 34 FF 00 00 EE  00 00 FF 02 FE D5 15 16 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_FRAME && log.data[0][1] == 0x15);

    /* wrong DCS -> BAD_CHECKSUM, the next frame is fine */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 06 FA D5 03 32 01 06 07 E9 00  00 00 FF 06 FA D5 03 32 01 06 07 E8 00", &log);
    CHECK(log.n == 2 && log.ev[0] == PN532_RX_BAD_CHECKSUM && log.ev[1] == PN532_RX_FRAME);

    /* wrong LCS -> skipped silently, resync on the following frame */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 06 FB D5 03 32 01 06 07 E8 00  00 00 FF 02 FE D5 15 16 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_FRAME && log.data[0][1] == 0x15);

    /* LCS failure where the broken byte starts the real frame */
    log.n = 0;
    feed_hex(&rx, "00 FF 07 00 00 FF 02 FE D5 15 16 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_FRAME && log.data[0][1] == 0x15);

    /* answer cut off: a new transaction resets the receiver */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 06 FA D5 03 32", &log);
    CHECK(log.n == 0);
    pn532_rx_reset(&rx);
    feed_hex(&rx, "00 00 FF 00 FF 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_ACK);

    /* frame longer than the buffer: TOO_LONG, then in sync again */
    uint8_t small[4];
    pn532_rx_init(&rx, small, sizeof small);
    log.n = 0;
    feed_hex(&rx, "00 00 FF 06 FA D5 03 32 01 06 07 E8 00  00 00 FF 02 FE D5 15 16 00", &log);
    CHECK(log.n == 2 && log.ev[0] == PN532_RX_TOO_LONG && log.ev[1] == PN532_RX_FRAME);

    /* extended frame with wrong LCS is ignored */
    pn532_rx_init(&rx, buf, sizeof buf);
    log.n = 0;
    feed_hex(&rx, "00 00 FF FF FF 00 03 FE D5 4B 00 E0 00  00 00 FF 00 FF 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_ACK);
}

static void test_list_target(void)
{
    uint8_t buf[64];
    pn532_rx_t rx;
    pn532_rx_init(&rx, buf, sizeof buf);
    feed_log_t log = { .n = 0 };
    pn532_target_a_t t;

    /* NTAG215: SENS_RES 00 44, SAK 00, 7-byte UID */
    feed_hex(&rx, "00 00 FF 0F F1 D5 4B 01 01 00 44 00 07 04 68 95 71 FA 5C 64 67 00", &log);
    CHECK(log.n == 1 && log.ev[0] == PN532_RX_FRAME && log.len[0] == 15);
    CHECK(pn532_parse_list_106a(log.data[0] + 2, log.len[0] - 2, &t) == 1);
    CHECK(t.tg == 1 && t.sens_res == 0x0044 && t.sel_res == 0x00 && t.uid_len == 7);
    CHECK(memcmp(t.uid, "\x04\x68\x95\x71\xFA\x5C\x64", 7) == 0);

    /* no target */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 03 FD D5 4B 00 E0 00", &log);
    CHECK(log.n == 1 && pn532_parse_list_106a(log.data[0] + 2, log.len[0] - 2, &t) == 0);

    /* MIFARE Classic, 4-byte UID, SAK 08 */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 0C F4 D5 4B 01 01 00 04 08 04 DE AD BE EF 96 00", &log);
    CHECK(log.n == 1 && pn532_parse_list_106a(log.data[0] + 2, log.len[0] - 2, &t) == 1);
    CHECK(t.sel_res == 0x08 && t.uid_len == 4 && memcmp(t.uid, "\xDE\xAD\xBE\xEF", 4) == 0);

    /* ISO 14443-4 card (SAK 20) with ATS 06 75 77 81 02 80 */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 15 EB D5 4B 01 01 03 44 20 07 04 11 22 33 44 55 66 06 75 77 81 02 80 12 00", &log);
    CHECK(log.n == 1 && pn532_parse_list_106a(log.data[0] + 2, log.len[0] - 2, &t) == 1);
    CHECK(t.sel_res == 0x20 && t.uid_len == 7 && t.uid[6] == 0x66);

    /* malformed answers */
    uint8_t p[32];
    size_t n = hexb("01 01 00 44 00 07 04 68 95 71 FA 5C", p);   /* UID cut short */
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
    n = hexb("01 01 00 44 00 05 04 68 95 71 FA", p);             /* UID length 5 */
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
    n = hexb("01 01 00 44 00 07 04 68 95 71 FA 5C 64 99", p);   /* trailing byte */
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
    n = hexb("01 01 03 44 20 07 04 11 22 33 44 55 66 06 75 77", p);   /* ATS cut */
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
    n = hexb("01 01 03 44 20 07 04 11 22 33 44 55 66 00", p);   /* ATS length 0 */
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
    n = hexb("00 00", p);
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
    CHECK(pn532_parse_list_106a(p, 0, &t) == -1);
    CHECK(pn532_parse_list_106a(NULL, 3, &t) == -1);
    n = hexb("01 01 00 44", p);
    CHECK(pn532_parse_list_106a(p, n, &t) == -1);
}

static void test_tag_answers(void)
{
    uint8_t buf[64];
    pn532_rx_t rx;
    pn532_rx_init(&rx, buf, sizeof buf);
    feed_log_t log = { .n = 0 };

    /* InDataExchange answer to READ: status 00 + 16 bytes */
    feed_hex(&rx, "00 00 FF 00 FF 00 00 00 FF 13 ED D5 41 00 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 72 00",
             &log);
    CHECK(log.n == 2 && log.ev[1] == PN532_RX_FRAME && log.len[1] == 19);
    CHECK(log.data[1][1] == 0x41 && log.data[1][2] == 0x00 && log.data[1][3] == 0x10 && log.data[1][18] == 0x1F);

    /* InDataExchange with status 01 (timeout: tag gone) */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 03 FD D5 41 01 E9 00", &log);
    CHECK(log.n == 1 && log.data[0][2] == 0x01);
    CHECK(strcmp(pn532_status_str(log.data[0][2]), "timeout (no answer from the tag)") == 0);

    /* InCommunicateThru answer to GET_VERSION (NTAG215) */
    log.n = 0;
    feed_hex(&rx, "00 00 FF 0B F5 D5 43 00 00 04 04 02 01 00 11 03 C9 00", &log);
    CHECK(log.n == 1 && log.len[0] == 11 && log.data[0][1] == 0x43 && log.data[0][9] == 0x11);

    CHECK(strcmp(pn532_status_str(0x00), "ok") == 0);
    CHECK(strcmp(pn532_status_str(0x40 | 0x14), "authentication error / tag NAK") == 0);   /* MI bit ignored */
    CHECK(strcmp(pn532_status_str(0x3F), "unknown PN532 error") == 0);
    for (unsigned s = 0; s < 256; s++) {
        CHECK(pn532_status_str((uint8_t)s) != NULL);
    }
}

/* Random byte streams: no crash/overflow (ASan), and a valid frame inserted
 * after arbitrary noise plus a reset is always found. */
static void test_fuzz(void)
{
    uint32_t seed = 12345;
    uint8_t buf[32];
    pn532_rx_t rx;
    pn532_rx_init(&rx, buf, sizeof buf);
    uint8_t frame[16];
    size_t fn = hexb("00 00 FF 06 FA D5 03 32 01 06 07 E8 00", frame);
    int found_total = 0;
    for (int iter = 0; iter < 20000; iter++) {
        uint8_t noise[64];
        size_t nn = (size_t)(iter % 64);
        for (size_t i = 0; i < nn; i++) {
            seed = seed * 1103515245u + 12345u;
            uint8_t r = (uint8_t)(seed >> 16);
            /* bias towards the interesting bytes */
            noise[i] = (r & 3) == 0 ? 0x00 : (r & 3) == 1 ? 0xFF : r;
        }
        for (size_t i = 0; i < nn; i++) {
            (void)pn532_rx_feed(&rx, noise[i]);
        }
        pn532_rx_reset(&rx);
        int found = 0;
        for (size_t i = 0; i < fn; i++) {
            if (pn532_rx_feed(&rx, frame[i]) == PN532_RX_FRAME && rx.len == 6 && buf[1] == 0x03) {
                found++;
            }
        }
        found_total += found == 1;
    }
    CHECK(found_total == 20000);
}

int main(void)
{
    test_build();
    test_receive_basic();
    test_receive_errors();
    test_list_target();
    test_tag_answers();
    test_fuzz();
    printf("pn532_frame: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
