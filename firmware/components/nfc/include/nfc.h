/*
 * nfc - PN532 NFC reader service (HSU/UART, 115200 baud) for Spotify tags.
 *
 * A task on core 0 polls the reader every poll interval: RF field on, look for
 * one ISO 14443 A tag, RF field off. A new tag is read (CC + as much of the
 * NDEF data as needed) and reported once with NFC_EVT_TAG_PRESENT; a tag left
 * on the reader is not reported again. NFC_EVT_TAG_REMOVED follows after three
 * polls in a row without the tag. When the PN532 stops answering the service
 * reports NFC_EVT_READER_ERROR and re-initialises it with back-off (1 s, 2 s,
 * 4 s ... 30 s); NFC_EVT_READER_OK reports that it is (again) ready.
 *
 * Pins: board_pins.h (BOARD_NFC_UART_TX_GPIO / _RX_GPIO / BOARD_NFC_RSTPD_N_GPIO).
 * Wiring and test steps: docs/NFC.md. Tag format: nfc_tag.h.
 *
 * Interface from docs/ARCHITECTURE.md section 3; everything marked
 * "addition" extends it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nfc_tag.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NFC_EVT_TAG_PRESENT,    /* a new tag lies on the reader                          */
    NFC_EVT_TAG_REMOVED,    /* the tag of the last TAG_PRESENT/TAG_WRITTEN is gone    */
    NFC_EVT_READER_ERROR,   /* PN532 not answering (once per failure period)          */
    /* additions */
    NFC_EVT_READER_OK,      /* PN532 initialised (at start and after a READER_ERROR)  */
    NFC_EVT_TAG_WRITTEN,    /* nfc_write_ref() succeeded: the tag lies on the reader
                               and holds ref. Takes the place of TAG_PRESENT for this
                               tag (no TAG_PRESENT follows while it stays);
                               TAG_REMOVED follows when it is lifted.                  */
} nfc_event_id_t;

typedef struct {
    nfc_event_id_t id;
    uint8_t        uid[7];      /* first 7 bytes of the UID (NTAG: all of it)          */
    uint8_t        uid_len;     /* 4 or 7 (10-byte UIDs are cut to 7)                  */
    bool           has_ref;     /* TAG_PRESENT/TAG_WRITTEN: ref is valid               */
    spotify_ref_t  ref;
    /* additions, TAG_PRESENT only */
    bool           read_ok;     /* tag memory was read; false: not a Type 2 tag or
                                   the tag could not be read (moved too fast)          */
    nfc_tag_err_t  tag_err;     /* read_ok && !has_ref: why there is no reference      */
} nfc_event_t;

/* Callback: runs on the nfc task. Keep it short (< 10 ms), copy what you need,
 * never call nfc_write_ref()/nfc_read_now()/nfc_stop() from it. */
typedef void (*nfc_event_cb_t)(const nfc_event_t *ev, void *ctx);

/* Opens the UART and starts the service task. The reader is initialised by
 * the task, so this returns at once; a missing PN532 shows up as
 * NFC_EVT_READER_ERROR. cb may be NULL. ESP_ERR_INVALID_STATE if running. */
esp_err_t nfc_start(void (*cb)(const nfc_event_t *, void *), void *ctx);

/* Waits up to timeout_ms for a tag (any tag on the reader counts, also one
 * that lies there already), writes ref as an https://open.spotify.com link,
 * reads it back and compares. Polling pauses meanwhile. Only NTAG213/215/216;
 * an unformatted NTAG (CC 00 00 00 00) gets its capability container first.
 *   ESP_OK                  written and verified (NFC_EVT_TAG_WRITTEN follows)
 *   ESP_ERR_INVALID_ARG     invalid ref
 *   ESP_ERR_INVALID_STATE   service not started, reader not ready, or called
 *                           from the event callback
 *   ESP_ERR_TIMEOUT         no tag within timeout_ms
 *   ESP_ERR_NOT_SUPPORTED   not an NTAG21x, or CC / control TLVs not usable
 *   ESP_ERR_NOT_ALLOWED     tag locked (lock bits, read-only CC) or password protected
 *   ESP_ERR_INVALID_SIZE    content does not fit the tag
 *   ESP_ERR_INVALID_CRC     read back differs (tag left with an empty or old message)
 *   ESP_FAIL                tag moved away during the write, or reader error
 * The reason as text: nfc_status_t.last_write_error. */
esp_err_t nfc_write_ref(const spotify_ref_t *ref, uint32_t timeout_ms);

/* Poll interval while idle; 300 by default, 750 recommended during playback.
 * Clamped to 50 .. 5000 ms. Takes effect after the current wait. */
void nfc_set_poll_interval_ms(uint32_t ms);

/* ---- additions ---------------------------------------------------------------- */

/* As nfc_write_ref() for a Spotify link or URI (nfc_tag_parse_link() forms);
 * ESP_ERR_INVALID_ARG if it is not one. */
esp_err_t nfc_write_url(const char *link, uint32_t timeout_ms);

/* Stops the task, switches the RF field off, holds the PN532 in reset (power
 * down) and releases the UART. No events afterwards. Safe if not running. */
void nfc_stop(void);

#define NFC_ERR_TEXT_MAX 64

typedef struct {
    bool          running;            /* service task active                         */
    bool          reader_ok;          /* PN532 answered the last command             */
    char          firmware[24];       /* e.g. "PN532 v1.6", "" before the first init  */
    uint32_t      poll_interval_ms;
    /* current tag (reported with TAG_PRESENT / TAG_WRITTEN, not yet removed) */
    bool          tag_present;
    uint8_t       uid[10];
    uint8_t       uid_len;
    uint8_t       sak;                /* SEL_RES: 0x00 = Type 2                       */
    uint16_t      model;              /* 213/215/216 from GET_VERSION, 0 = unknown     */
    bool          has_ref;
    spotify_ref_t ref;
    char          tag_error[NFC_ERR_TEXT_MAX];        /* why has_ref is false          */
    char          last_error[NFC_ERR_TEXT_MAX];       /* last reader/communication error */
    char          last_write_error[NFC_ERR_TEXT_MAX]; /* reason of the last failed write */
    /* counters since nfc_start() */
    uint32_t      polls;
    uint32_t      tags_detected;      /* TAG_PRESENT events                           */
    uint32_t      read_errors;        /* tag reads that failed on the radio link       */
    uint32_t      comm_errors;        /* PN532 commands without a valid answer         */
    uint32_t      reader_inits;       /* successful (re)initialisations                */
    uint32_t      writes_ok, writes_failed;
} nfc_status_t;

/* Snapshot of the service state (thread-safe; zeroed if never started). */
void nfc_get_status(nfc_status_t *out);

/* One-off read for diagnostics: waits up to timeout_ms for a tag and reads it,
 * independent of the poll loop (which pauses meanwhile; it does not report
 * anything for this read). */
typedef struct {
    uint8_t       uid[10];
    uint8_t       uid_len;
    uint8_t       sak;
    uint16_t      model;              /* 0 if GET_VERSION failed (not an NTAG21x)      */
    size_t        bytes_read;         /* data-area bytes read                         */
    bool          has_ref;
    spotify_ref_t ref;
    nfc_tag_err_t tag_err;            /* !has_ref and the memory was read              */
    char          link[NFC_TAG_LINK_MAX]; /* link text found on the tag, may be ""     */
    char          error[NFC_ERR_TEXT_MAX]; /* "" or the reason when !has_ref           */
} nfc_read_result_t;

/* ESP_OK: a tag was read (see has_ref / error); ESP_ERR_TIMEOUT: no tag;
 * ESP_ERR_INVALID_STATE: not running / reader not ready; ESP_FAIL: the tag
 * could not be read; ESP_ERR_NO_MEM. */
esp_err_t nfc_read_now(nfc_read_result_t *out, uint32_t timeout_ms);

/* Short name for an event id ("TAG_PRESENT", ...). */
const char *nfc_event_name(nfc_event_id_t id);

#ifdef __cplusplus
}
#endif
