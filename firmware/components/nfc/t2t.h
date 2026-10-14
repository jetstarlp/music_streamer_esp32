/*
 * Reading and writing Spotify references on NFC Forum Type 2 tags (NTAG21x)
 * through an abstract page I/O interface. Pure C99 without ESP-IDF
 * dependencies: nfc.c plugs in the PN532, the host tests a simulated tag
 * (tests/host/nfc). All tag-format decisions come from nfc_tag.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nfc_tag.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I/O callbacks: return T2T_IO_OK or one of the failure classes. */
typedef enum {
    T2T_IO_OK = 0,
    T2T_IO_TAG,      /* the tag did not answer / NAKed (moved away, protected page) */
    T2T_IO_READER,   /* no valid answer from the reader                             */
} t2t_io_status_t;

typedef struct {
    /* READ (0x30): 16 bytes = pages page .. page+3 (the tag rolls over at the end). */
    t2t_io_status_t (*read16)(void *ctx, uint16_t page, uint8_t out[16]);
    /* WRITE (0xA2): one page. */
    t2t_io_status_t (*write4)(void *ctx, uint16_t page, const uint8_t data[4]);
    /* GET_VERSION (0x60): 8 bytes. */
    t2t_io_status_t (*get_version)(void *ctx, uint8_t out[8]);
    void *ctx;
} t2t_io_t;

typedef enum {
    T2T_OK = 0,
    T2T_ERR_IO,       /* see io                                                    */
    T2T_ERR_TAG,      /* tag content / layout / protection: see tag_err            */
    T2T_ERR_VERIFY,   /* write: read back differs from what was written            */
} t2t_result_t;

typedef struct {
    t2t_result_t    res;
    t2t_io_status_t io;          /* for T2T_ERR_IO                                 */
    nfc_tag_err_t   tag_err;     /* for T2T_ERR_TAG                                */
    uint16_t        model;       /* write: 213/215/216 from GET_VERSION, else 0    */
    bool            cc_written;  /* write: the tag was unformatted, CC written     */
    size_t          bytes_read;  /* read: data-area bytes read                     */
    unsigned        reads;       /* number of read16 calls                         */
    unsigned        writes;      /* number of write4 calls                         */
} t2t_outcome_t;

/* Reads the CC and, incrementally (nfc_tag_ndef_bytes_needed), as much of the
 * data area as the first NDEF TLV needs (at most min(data size from the CC,
 * cap) bytes into buf), then parses it with nfc_tag_parse_ndef_ex().
 * T2T_OK: *ref is set. T2T_ERR_TAG: the content holds no usable reference
 * (tag_err says why; link receives the link text if there was one). */
t2t_result_t t2t_read_ref(const t2t_io_t *io, uint8_t *buf, size_t cap, spotify_ref_t *ref,
                          char *link, size_t link_cap, t2t_outcome_t *out);

/* Capability container for an unformatted NTAG21x (E1 10 <size/8> 00). */
bool t2t_ntag_default_cc(uint16_t model, uint8_t cc[4]);

/* Writes ref to an NTAG213/215/216:
 *   GET_VERSION -> model; page 2 static lock bits; CC (an all-zero CC is
 *   written for the model first); data area head (control TLVs); dynamic lock
 *   bits and AUTH0; then the nfc_tag_t2t_plan_write() order: length byte 0,
 *   rest, real length; finally read back and compare. */
t2t_result_t t2t_write_ref(const t2t_io_t *io, const spotify_ref_t *ref, t2t_outcome_t *out);

#ifdef __cplusplus
}
#endif
