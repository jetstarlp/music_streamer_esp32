/*
 * nfc_tag - Spotify links on NFC Forum Type 2 tags (NTAG213/215/216).
 *
 * Pure C99 without ESP-IDF or hardware dependencies; tested on the PC in
 * tests/host/nfc_tag. Nothing here talks to a reader: the functions work on
 * memory images that the nfc component has read from (or will write to) a tag.
 *
 * Tag layout (NFC Forum Type 2 Tag, NTAG215 as example):
 *   page 0..2   UID, static lock bytes (page 2, bytes 2..3)
 *   page 3      capability container (CC), E1 10 3E 00 = NDEF 1.0, 496 bytes, writable
 *   page 4..    data area: TLV blocks. 03 = NDEF message, FE = terminator,
 *               00 = NULL (padding), 01/02 = lock/memory control.
 *   page 0x82.. dynamic lock bytes, configuration (NTAG215)
 *
 * Content written by this module (and by phone apps such as "NFC Tools"):
 *   03 <len> D1 01 <plen> 55 04 "open.spotify.com/<type>/<id>" FE
 *   i.e. one NDEF URI record with prefix code 0x04 ("https://"). Phones open
 *   such https links in the Spotify app; spotify: URIs open only the Spotify
 *   home screen on Android, so tags always carry the https form.
 *
 * Thread safety: all functions are re-entrant except that the bool API stores
 * its failure reason in one process-wide variable (nfc_tag_last_error()).
 * Tasks running in parallel use the *_ex variants, which return the reason.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Spotify references (docs/ARCHITECTURE.md section 3) ------------------ */

typedef enum { SPOTIFY_REF_TRACK, SPOTIFY_REF_ALBUM, SPOTIFY_REF_PLAYLIST,
               SPOTIFY_REF_ARTIST, SPOTIFY_REF_SHOW, SPOTIFY_REF_EPISODE } spotify_ref_type_t;
typedef struct { spotify_ref_type_t type; char id[23]; } spotify_ref_t;   /* 22 base62 characters */

#define SPOTIFY_REF_ID_LEN      22
#define SPOTIFY_REF_URI_MAX     40   /* "spotify:playlist:" + 22 + NUL                    */
#define SPOTIFY_REF_URL_MAX     57   /* "https://open.spotify.com/playlist/" + 22 + NUL   */

/* ---- Failure reasons ------------------------------------------------------- */

typedef enum {
    NFC_TAG_OK = 0,
    NFC_TAG_ERR_ARG,              /* NULL pointer, invalid reference or argument      */
    NFC_TAG_ERR_NO_NDEF,          /* no NDEF TLV before the terminator / end of data  */
    NFC_TAG_ERR_EMPTY,            /* empty NDEF message (factory state 03 00 FE)      */
    NFC_TAG_ERR_TLV,              /* malformed TLV (reserved length 0xFFFF)           */
    NFC_TAG_ERR_TRUNCATED,        /* a TLV runs past the given memory: read more      */
    NFC_TAG_ERR_RECORD,           /* malformed NDEF record or message                 */
    NFC_TAG_ERR_CHUNKED,          /* chunked NDEF records are not supported           */
    NFC_TAG_ERR_NO_LINK,          /* no URI record and no link in a Text record       */
    NFC_TAG_ERR_NOT_SPOTIFY,      /* a link, but not to Spotify                       */
    NFC_TAG_ERR_SHORT_LINK,       /* spotify.link short link: needs a network lookup  */
    NFC_TAG_ERR_UNSUPPORTED_TYPE, /* Spotify link of another kind (user, genre, ...)  */
    NFC_TAG_ERR_BAD_ID,           /* id is not exactly 22 base62 characters           */
    NFC_TAG_ERR_NO_SPACE,         /* output buffer or tag data area too small         */
    NFC_TAG_ERR_NOT_FORMATTED,    /* Type 2: no NDEF capability container             */
    NFC_TAG_ERR_READ_ONLY,        /* Type 2: CC write access denied or lock bits set  */
    NFC_TAG_ERR_PASSWORD,         /* NTAG: pages to write are password protected      */
    NFC_TAG_ERR_UNSUPPORTED,      /* tag type, version or layout not handled          */
} nfc_tag_err_t;

/* Short English description, never NULL. */
const char *nfc_tag_err_str(nfc_tag_err_t err);

/* Reason of the most recent call of nfc_tag_parse_ndef(), nfc_tag_parse_link()
 * or nfc_tag_build_ndef() in the whole process (NFC_TAG_OK after a success).
 * Not synchronised between tasks; see the *_ex functions. */
nfc_tag_err_t nfc_tag_last_error(void);

/* ---- Interface from docs/ARCHITECTURE.md section 3 ------------------------- */

/* Finds a Spotify reference in Type 2 tag memory. mem starts at page 4 (the
 * first byte of the data area), len is the number of bytes available.
 * Evaluates the first NDEF TLV: URI records (all prefix codes 0x00..0x23),
 * Text records containing a link, absolute-URI records and URI records nested
 * in a Smart Poster. The first record that holds a valid link wins.
 * *out is written only on success (same for all parse functions). */
bool   nfc_tag_parse_ndef(const uint8_t *mem, size_t len, spotify_ref_t *out);

/* Parses "https://open.spotify.com/[intl-xx/]<type>/<id>[?...]" (scheme and
 * query optional) or "spotify:<type>:<id>". Leading/trailing white space is
 * ignored. Types: track, album, playlist, artist, show, episode. The legacy
 * forms ".../user/<name>/playlist/<id>" and "spotify:user:<name>:playlist:<id>"
 * are accepted as well. spotify.link short links fail with
 * NFC_TAG_ERR_SHORT_LINK (they can only be resolved online). */
bool   nfc_tag_parse_link(const char *text, spotify_ref_t *out);

/* Builds the tag content for ref, to be written from page 4 on:
 * NDEF TLV + one URI record ("https://open.spotify.com/<type>/<id>") + FE.
 * Returns the number of bytes (at most NFC_TAG_NDEF_BUILD_MAX), 0 on error. */
size_t nfc_tag_build_ndef(const spotify_ref_t *ref, uint8_t *out, size_t cap);

/* "spotify:<type>:<id>". cap >= SPOTIFY_REF_URI_MAX is always enough. An
 * invalid ref or a too small buffer give "" (when cap > 0). */
void   spotify_ref_to_uri(const spotify_ref_t *ref, char *out, size_t cap);

/* ---- Additions -------------------------------------------------------------- */

#define NFC_TAG_NDEF_BUILD_MAX  56   /* longest output of nfc_tag_build_ndef()        */
#define NFC_TAG_LINK_MAX        256  /* link buffer for nfc_tag_parse_ndef_ex(): enough
                                         for share links in practice; a longer link is
                                         only truncated in the copy, never when parsed */

/* "https://open.spotify.com/<type>/<id>". cap >= SPOTIFY_REF_URL_MAX is enough;
 * "" on error (when cap > 0). */
void   spotify_ref_to_url(const spotify_ref_t *ref, char *out, size_t cap);

/* "track", "album", ...; NULL for a value outside the enum. */
const char *spotify_ref_type_name(spotify_ref_type_t type);

/* Type inside the enum and id of exactly 22 base62 characters plus NUL. */
bool   spotify_ref_is_valid(const spotify_ref_t *ref);

/* Both valid and equal in type and id. */
bool   spotify_ref_equal(const spotify_ref_t *a, const spotify_ref_t *b);

/* As nfc_tag_parse_ndef(), returning the reason. If link is not NULL it
 * receives the link text that decided the result (URI with its prefix
 * expanded, or the link found in a Text record), truncated to link_cap - 1
 * characters, or "" if there was none. */
nfc_tag_err_t nfc_tag_parse_ndef_ex(const uint8_t *mem, size_t len, spotify_ref_t *out,
                                    char *link, size_t link_cap);

/* As nfc_tag_parse_link(), returning the reason. */
nfc_tag_err_t nfc_tag_parse_link_ex(const char *text, spotify_ref_t *out);

/* As nfc_tag_build_ndef(), returning the reason; *out_len = bytes written. */
nfc_tag_err_t nfc_tag_build_ndef_ex(const spotify_ref_t *ref, uint8_t *out, size_t cap,
                                    size_t *out_len);

/* Incremental reading: given the first len bytes of the data area, returns how
 * many bytes are needed to evaluate the first NDEF TLV. A result <= len means
 * "enough, parse now" (nfc_tag_parse_ndef_ex() will then not report
 * NFC_TAG_ERR_TRUNCATED); a result > len means "read more". The caller stops
 * at the data area size from the CC: a TLV length may point beyond it, and
 * a data area without terminator and NDEF TLV keeps asking for one more byte. */
size_t nfc_tag_ndef_bytes_needed(const uint8_t *mem, size_t len);

/* ---- Type 2 tag (NTAG21x) helpers for the writer ---------------------------- */

#define NFC_TAG_T2T_PAGE_SIZE       4
#define NFC_TAG_T2T_FIRST_DATA_PAGE 4
#define NFC_TAG_T2T_PLAN_MAX        96   /* bytes of nfc_tag_t2t_plan_t.img           */
#define NFC_TAG_T2T_PLAN_READ       32   /* data-area bytes to read before planning   */

/* Checks the capability container (page 3). Reading needs the NDEF magic
 * number E1, mapping version 1.x and read access 0; writing additionally needs
 * write access 0. *data_size (optional) = data area size in bytes (CC byte 2 * 8). */
nfc_tag_err_t nfc_tag_t2t_check_cc(const uint8_t cc[4], bool for_write, size_t *data_size);

/* Static lock bytes in page 2 (bytes 2 and 3): any bit set gives
 * NFC_TAG_ERR_READ_ONLY. This module never sets lock bits. */
nfc_tag_err_t nfc_tag_t2t_check_static_lock(const uint8_t page2[4]);

typedef struct {
    uint16_t model;          /* 213, 215 or 216                                     */
    uint16_t total_pages;    /* 45, 135, 231                                        */
    uint16_t user_bytes;     /* 144, 504, 888 (pages 4 .. dyn_lock_page - 1)        */
    uint16_t dyn_lock_page;  /* 0x28, 0x82, 0xE2; CFG0, CFG1, PWD follow            */
} nfc_tag_ntag_info_t;

/* Interprets the 8-byte answer to GET_VERSION (0x60). NXP NTAG213/215/216 only
 * (vendor 0x04, product type 0x04); anything else is NFC_TAG_ERR_UNSUPPORTED. */
nfc_tag_err_t nfc_tag_ntag_from_version(const uint8_t ver[8], nfc_tag_ntag_info_t *info);

/* Checks the 16 bytes read from info->dyn_lock_page (dynamic lock bytes, CFG0,
 * CFG1, PWD): any dynamic lock bit set gives NFC_TAG_ERR_READ_ONLY; AUTH0 at or
 * below last_page (the last page to be written) gives NFC_TAG_ERR_PASSWORD. */
nfc_tag_err_t nfc_tag_ntag_check_config(const nfc_tag_ntag_info_t *info, const uint8_t cfg[16],
                                        uint16_t last_page);

/* A safe NDEF write (NFC Forum T2T, "NDEF write procedure"):
 *   1. pages first_page .. len_page with the TLV length byte set to 0
 *      (the tag holds an empty NDEF message while the rest is written),
 *   2. pages len_page + 1 .. last page,
 *   3. page len_page again with the real length,
 *   4. read back img[0 .. end) from page 4 and compare.
 * img holds the complete new content of the data area bytes 0 .. end-1:
 * control TLVs found in front of the NDEF TLV (NTAG213 ships with a Lock
 * Control TLV) are kept, the NDEF TLV follows them, the rest of the last page
 * is padded with 0x00. Page numbers are absolute (data area starts at page 4). */
typedef struct {
    uint8_t  img[NFC_TAG_T2T_PLAN_MAX];
    size_t   end;            /* bytes of img to write and verify, multiple of 4     */
    size_t   ndef_offset;    /* offset of the NDEF TLV (T byte) in the data area     */
    size_t   len_offset;     /* offset of the TLV length byte                        */
    uint16_t first_page;     /* first page to write                                  */
    uint16_t len_page;       /* page holding the length byte                         */
    uint16_t last_page;      /* last page to write                                   */
} nfc_tag_t2t_plan_t;

/* cur: the current first cur_len bytes of the data area (at least the control
 * TLVs; NFC_TAG_T2T_PLAN_READ bytes are always enough for NTAG21x).
 * data_size: from nfc_tag_t2t_check_cc(). Fails with NFC_TAG_ERR_NO_SPACE if
 * the content does not fit, NFC_TAG_ERR_UNSUPPORTED if a control TLV is
 * malformed or reserves bytes inside the range to be written, and
 * NFC_TAG_ERR_TRUNCATED if cur ends inside the control TLVs. */
nfc_tag_err_t nfc_tag_t2t_plan_write(const uint8_t *cur, size_t cur_len, size_t data_size,
                                     const spotify_ref_t *ref, nfc_tag_t2t_plan_t *plan);

#ifdef __cplusplus
}
#endif
