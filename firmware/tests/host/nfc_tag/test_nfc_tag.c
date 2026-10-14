/*
 * Host tests for components/nfc_tag (pure C, no ESP-IDF).
 * Run: firmware/tools/host_test_nfc_tag.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc_tag.h"

static int s_checks, s_fail;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        s_checks++;                                                                  \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            s_fail++;                                                                \
        }                                                                            \
    } while (0)

#define CHECK_ERR(got, want)                                                         \
    do {                                                                             \
        nfc_tag_err_t g_ = (got), w_ = (want);                                       \
        s_checks++;                                                                  \
        if (g_ != w_) {                                                              \
            fprintf(stderr, "%s:%d: %s = %s, expected %s\n", __FILE__, __LINE__, #got, \
                    nfc_tag_err_str(g_), nfc_tag_err_str(w_));                       \
            s_fail++;                                                                \
        }                                                                            \
    } while (0)

/* Sample ids (22 base62 characters). */
#define ID_PL  "37i9dQZF1DXcBWIGoYBM5M"
#define ID_TR  "4uLU6hMCjMI75M1A2tKUQC"
#define ID_AL  "1DFixLWuPkv3KT3TnV35m3"
#define ID_AR  "0OdUWJ0sBjDrqHygGUXeCF"
#define ID_SH  "4rOoJ6Egrf8K2IrywzwOMk"
#define ID_EP  "512ojhOuo1ktJprKbVcKyQ"

/* ---- Builders for test images --------------------------------------------- */

/* "03 3C D1 ..." -> bytes; returns the count. */
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

#define F_MB 0x80
#define F_ME 0x40
#define F_CF 0x20
#define F_SR 0x10
#define F_IL 0x08

/* Appends one NDEF record (short form when the payload is below 256 bytes
 * unless force_long). Returns the number of bytes. */
static size_t rec_ex(uint8_t *p, uint8_t flags, int tnf, const char *type, size_t tl,
                     const char *id, size_t il, const uint8_t *pay, size_t pl, int force_long)
{
    size_t n = 0;
    int sr = pl < 256 && !force_long;
    p[n++] = (uint8_t)(flags | (sr ? F_SR : 0) | (il ? F_IL : 0) | tnf);
    p[n++] = (uint8_t)tl;
    if (sr) {
        p[n++] = (uint8_t)pl;
    } else {
        p[n++] = (uint8_t)(pl >> 24);
        p[n++] = (uint8_t)(pl >> 16);
        p[n++] = (uint8_t)(pl >> 8);
        p[n++] = (uint8_t)pl;
    }
    if (il) {
        p[n++] = (uint8_t)il;
    }
    memcpy(p + n, type, tl);
    n += tl;
    memcpy(p + n, id, il);
    n += il;
    memcpy(p + n, pay, pl);
    return n + pl;
}

static size_t rec(uint8_t *p, uint8_t flags, int tnf, const char *type, const uint8_t *pay, size_t pl)
{
    return rec_ex(p, flags, tnf, type, strlen(type), "", 0, pay, pl, 0);
}

static size_t uri_payload(uint8_t *p, uint8_t code, const char *rest)
{
    p[0] = code;
    memcpy(p + 1, rest, strlen(rest));
    return 1 + strlen(rest);
}

static size_t text_payload(uint8_t *p, const char *lang, const char *text)
{
    size_t ll = strlen(lang), tl = strlen(text);
    p[0] = (uint8_t)ll;
    memcpy(p + 1, lang, ll);
    memcpy(p + 1 + ll, text, tl);
    return 1 + ll + tl;
}

/* pre (control TLVs) + NDEF TLV around msg (long length form when needed or
 * forced) + FE + zero padding to a multiple of 16. */
static size_t tlv_ex(uint8_t *out, const char *pre_hex, const uint8_t *msg, size_t n, int force_long)
{
    size_t k = pre_hex ? hexb(pre_hex, out) : 0;
    out[k++] = 0x03;
    if (n >= 0xFF || force_long) {
        out[k++] = 0xFF;
        out[k++] = (uint8_t)(n >> 8);
        out[k++] = (uint8_t)n;
    } else {
        out[k++] = (uint8_t)n;
    }
    memcpy(out + k, msg, n);
    k += n;
    out[k++] = 0xFE;
    while (k % 16) {
        out[k++] = 0;
    }
    return k;
}

static size_t tlv(uint8_t *out, const uint8_t *msg, size_t n)
{
    return tlv_ex(out, NULL, msg, n, 0);
}

/* Tag image with one URI record. */
static size_t uri_image(uint8_t *out, uint8_t code, const char *rest)
{
    uint8_t pay[512], msg[600];
    size_t pl = uri_payload(pay, code, rest);
    size_t ml = rec(msg, F_MB | F_ME, 1, "U", pay, pl);
    return tlv(out, msg, ml);
}

static nfc_tag_err_t parse(const uint8_t *mem, size_t len, spotify_ref_t *ref)
{
    return nfc_tag_parse_ndef_ex(mem, len, ref, NULL, 0);
}

static int ref_is(const spotify_ref_t *r, spotify_ref_type_t t, const char *id)
{
    return r->type == t && strcmp(r->id, id) == 0;
}

/* ---- Spotify references ------------------------------------------------------ */

static void test_err_str(void)
{
    for (int e = NFC_TAG_OK; e <= NFC_TAG_ERR_UNSUPPORTED; e++) {
        const char *s = nfc_tag_err_str((nfc_tag_err_t)e);
        CHECK(s && *s && strcmp(s, "unknown error") != 0);
        for (int f = NFC_TAG_OK; f < e; f++) {
            CHECK(strcmp(s, nfc_tag_err_str((nfc_tag_err_t)f)) != 0);
        }
    }
    CHECK(strcmp(nfc_tag_err_str((nfc_tag_err_t)99), "unknown error") == 0);
}

static void test_ref_helpers(void)
{
    static const char *names[] = { "track", "album", "playlist", "artist", "show", "episode" };
    for (int t = 0; t < 6; t++) {
        CHECK(strcmp(spotify_ref_type_name((spotify_ref_type_t)t), names[t]) == 0);
    }
    CHECK(spotify_ref_type_name((spotify_ref_type_t)6) == NULL);
    CHECK(spotify_ref_type_name((spotify_ref_type_t)-1) == NULL);

    spotify_ref_t r = { SPOTIFY_REF_PLAYLIST, ID_PL }, q = r;
    CHECK(spotify_ref_is_valid(&r));
    CHECK(!spotify_ref_is_valid(NULL));
    CHECK(spotify_ref_equal(&r, &q));
    q.type = SPOTIFY_REF_ALBUM;
    CHECK(!spotify_ref_equal(&r, &q));
    q = r;
    q.id[5] = 'x';
    CHECK(!spotify_ref_equal(&r, &q));
    CHECK(!spotify_ref_equal(&r, NULL) && !spotify_ref_equal(NULL, NULL));
    q = r;
    q.type = (spotify_ref_type_t)6;
    CHECK(!spotify_ref_is_valid(&q) && !spotify_ref_equal(&q, &q));
    q = r;
    q.id[21] = '-';
    CHECK(!spotify_ref_is_valid(&q));
    q = r;
    q.id[21] = '\0';
    CHECK(!spotify_ref_is_valid(&q));
    memset(&q, 0xFF, sizeof q);
    CHECK(!spotify_ref_is_valid(&q));

    char buf[SPOTIFY_REF_URL_MAX];
    spotify_ref_to_uri(&r, buf, SPOTIFY_REF_URI_MAX);
    CHECK(strcmp(buf, "spotify:playlist:" ID_PL) == 0);
    CHECK(strlen(buf) + 1 == SPOTIFY_REF_URI_MAX);
    spotify_ref_to_url(&r, buf, SPOTIFY_REF_URL_MAX);
    CHECK(strcmp(buf, "https://open.spotify.com/playlist/" ID_PL) == 0);
    CHECK(strlen(buf) + 1 == SPOTIFY_REF_URL_MAX);
    strcpy(buf, "x");
    spotify_ref_to_url(&r, buf, SPOTIFY_REF_URL_MAX - 1);   /* one byte short */
    CHECK(buf[0] == '\0');
    strcpy(buf, "x");
    spotify_ref_to_uri(&r, buf, SPOTIFY_REF_URI_MAX - 1);
    CHECK(buf[0] == '\0');
    strcpy(buf, "x");
    spotify_ref_to_uri(&q, buf, sizeof buf);                /* invalid ref */
    CHECK(buf[0] == '\0');
    strcpy(buf, "x");
    spotify_ref_to_uri(&r, buf, 0);                         /* cap 0: untouched */
    CHECK(buf[0] == 'x');
    spotify_ref_to_uri(&r, NULL, 10);
    spotify_ref_to_url(NULL, NULL, 0);
    spotify_ref_t e = { SPOTIFY_REF_EPISODE, ID_EP };
    spotify_ref_to_uri(&e, buf, sizeof buf);
    CHECK(strcmp(buf, "spotify:episode:" ID_EP) == 0);
}

/* ---- Link parser ---------------------------------------------------------------- */

typedef struct {
    const char        *text;
    nfc_tag_err_t      err;
    spotify_ref_type_t type;
    const char        *id;
} link_case_t;

static const link_case_t s_links[] = {
    { "https://open.spotify.com/playlist/" ID_PL, NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "http://open.spotify.com/track/" ID_TR, NFC_TAG_OK, SPOTIFY_REF_TRACK, ID_TR },
    { "open.spotify.com/album/" ID_AL, NFC_TAG_OK, SPOTIFY_REF_ALBUM, ID_AL },
    { "https://open.spotify.com/intl-de/artist/" ID_AR, NFC_TAG_OK, SPOTIFY_REF_ARTIST, ID_AR },
    { "https://open.spotify.com/intl-pt/track/" ID_TR "?si=a1b2c3&utm_source=copy-link",
      NFC_TAG_OK, SPOTIFY_REF_TRACK, ID_TR },
    { "https://open.spotify.com/show/" ID_SH "/", NFC_TAG_OK, SPOTIFY_REF_SHOW, ID_SH },
    { "https://open.spotify.com/episode/" ID_EP "#t=30", NFC_TAG_OK, SPOTIFY_REF_EPISODE, ID_EP },
    { "https://open.spotify.com/playlist/" ID_PL "/?si=x", NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "  \thttps://open.spotify.com/playlist/" ID_PL "?si=abc \r\n", NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "HTTPS://OPEN.SPOTIFY.COM/TRACK/" ID_TR, NFC_TAG_OK, SPOTIFY_REF_TRACK, ID_TR },
    { "https://open.spotify.com/embed/playlist/" ID_PL, NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "https://play.spotify.com/album/" ID_AL, NFC_TAG_OK, SPOTIFY_REF_ALBUM, ID_AL },
    { "open.spotify.com//track//" ID_TR, NFC_TAG_OK, SPOTIFY_REF_TRACK, ID_TR },
    { "https://open.spotify.com/user/spotify/playlist/" ID_PL, NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "https://open.spotify.com/user/1234567890/playlist/" ID_PL "?si=q", NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "spotify:track:" ID_TR, NFC_TAG_OK, SPOTIFY_REF_TRACK, ID_TR },
    { "spotify:playlist:" ID_PL, NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },
    { "spotify:album:" ID_AL, NFC_TAG_OK, SPOTIFY_REF_ALBUM, ID_AL },
    { "spotify:artist:" ID_AR, NFC_TAG_OK, SPOTIFY_REF_ARTIST, ID_AR },
    { "spotify:show:" ID_SH, NFC_TAG_OK, SPOTIFY_REF_SHOW, ID_SH },
    { "spotify:episode:" ID_EP, NFC_TAG_OK, SPOTIFY_REF_EPISODE, ID_EP },
    { "SPOTIFY:ALBUM:" ID_AL, NFC_TAG_OK, SPOTIFY_REF_ALBUM, ID_AL },
    { " spotify:user:spotify:playlist:" ID_PL "\n", NFC_TAG_OK, SPOTIFY_REF_PLAYLIST, ID_PL },

    { "", NFC_TAG_ERR_NO_LINK, 0, NULL },
    { " \t\r\n", NFC_TAG_ERR_NO_LINK, 0, NULL },
    { "hello", NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "https://example.com/playlist/" ID_PL, NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "https://notspotify.com/track/" ID_TR, NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "https://open.spotify.com.evil.example/track/" ID_TR, NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "ftp://open.spotify.com/track/" ID_TR, NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "tel:+491701234567", NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "https://", NFC_TAG_ERR_NOT_SPOTIFY, 0, NULL },
    { "https://spotify.link/AbCdEf12345", NFC_TAG_ERR_SHORT_LINK, 0, NULL },
    { "spotify.link/AbCdEf12345", NFC_TAG_ERR_SHORT_LINK, 0, NULL },
    { "https://spoti.fi/3xYz", NFC_TAG_ERR_SHORT_LINK, 0, NULL },
    { "https://spotify.app.link/xyz?_p=1", NFC_TAG_ERR_SHORT_LINK, 0, NULL },
    { "https://open.spotify.com/user/spotify", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/user/spotify/collection", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/genre/pop", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/collection/tracks", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/intl-de/", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/artist/" ID_AR "/discography", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://www.spotify.com/de/premium/", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "spotify:user:spotify", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "spotify:user:spotify:collection", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "spotify:genre:pop", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "spotify:", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "spotify:track:" ID_TR ":extra", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "spotify:local:a:b:c:1:2:3:4:5", NFC_TAG_ERR_UNSUPPORTED_TYPE, 0, NULL },
    { "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQ", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "https://open.spotify.com/track/" ID_TR "X", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKU-C", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "https://open.spotify.com/track/", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "https://open.spotify.com/track/" ID_TR " extra", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "spotify:track:", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "spotify:track", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "spotify:user:spotify:playlist", NFC_TAG_ERR_BAD_ID, 0, NULL },
    { "spotify:track:4uLU6hMCjMI75M1A2tKUQ\xC3\xA4", NFC_TAG_ERR_BAD_ID, 0, NULL },
};

static void test_parse_link(void)
{
    for (size_t i = 0; i < sizeof s_links / sizeof s_links[0]; i++) {
        const link_case_t *c = &s_links[i];
        spotify_ref_t r = { SPOTIFY_REF_TRACK, "untouched" };
        nfc_tag_err_t e = nfc_tag_parse_link_ex(c->text, &r);
        if (e != c->err) {
            fprintf(stderr, "link \"%s\": %s, expected %s\n", c->text, nfc_tag_err_str(e),
                    nfc_tag_err_str(c->err));
        }
        CHECK(e == c->err);
        if (c->err == NFC_TAG_OK) {
            CHECK(ref_is(&r, c->type, c->id));
            CHECK(spotify_ref_is_valid(&r));
        } else {
            CHECK(strcmp(r.id, "untouched") == 0);   /* out only written on success */
        }
        /* bool API + last error */
        CHECK(nfc_tag_parse_link(c->text, &r) == (c->err == NFC_TAG_OK));
        CHECK_ERR(nfc_tag_last_error(), c->err);
    }
    spotify_ref_t r;
    CHECK_ERR(nfc_tag_parse_link_ex(NULL, &r), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_parse_link_ex("spotify:track:" ID_TR, NULL), NFC_TAG_ERR_ARG);
    CHECK(!nfc_tag_parse_link(NULL, &r));
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_ERR_ARG);

    /* A link longer than NFC_TAG_LINK_MAX still parses. */
    char longl[700] = "https://open.spotify.com/playlist/" ID_PL "?si=";
    size_t n = strlen(longl);
    memset(longl + n, 'q', 600);
    longl[n + 600] = '\0';
    CHECK_ERR(nfc_tag_parse_link_ex(longl, &r), NFC_TAG_OK);
}

/* ---- Builder ------------------------------------------------------------------------ */

static void test_build(void)
{
    spotify_ref_t r = { SPOTIFY_REF_PLAYLIST, ID_PL };
    uint8_t buf[128], exp[128];
    size_t n = nfc_tag_build_ndef(&r, buf, sizeof buf);
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_OK);
    size_t en = hexb("03 35 D1 01 31 55 04", exp);
    memcpy(exp + en, "open.spotify.com/playlist/" ID_PL, 48);
    en += 48;
    exp[en++] = 0xFE;
    CHECK(n == en && n == NFC_TAG_NDEF_BUILD_MAX);
    CHECK(memcmp(buf, exp, en) == 0);

    /* Exact capacity works, one byte less does not. */
    CHECK(nfc_tag_build_ndef(&r, buf, n) == n);
    memset(buf, 0xAA, sizeof buf);
    CHECK(nfc_tag_build_ndef(&r, buf, n - 1) == 0);
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_ERR_NO_SPACE);
    CHECK(buf[0] == 0xAA);                        /* nothing written */

    size_t len = 99;
    CHECK_ERR(nfc_tag_build_ndef_ex(&r, buf, 10, &len), NFC_TAG_ERR_NO_SPACE);
    CHECK(len == 0);
    CHECK_ERR(nfc_tag_build_ndef_ex(NULL, buf, sizeof buf, &len), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_build_ndef_ex(&r, NULL, sizeof buf, &len), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_build_ndef_ex(&r, buf, sizeof buf, NULL), NFC_TAG_OK);
    spotify_ref_t bad = r;
    bad.id[3] = '/';
    CHECK(nfc_tag_build_ndef(&bad, buf, sizeof buf) == 0);
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_ERR_ARG);

    /* Every type: lengths and the build -> parse round trip. */
    static const char *ids[] = { ID_TR, ID_AL, ID_PL, ID_AR, ID_SH, ID_EP };
    for (int t = 0; t < 6; t++) {
        spotify_ref_t s = { (spotify_ref_type_t)t, "" }, p;
        memcpy(s.id, ids[t], 23);
        CHECK_ERR(nfc_tag_build_ndef_ex(&s, buf, sizeof buf, &len), NFC_TAG_OK);
        CHECK(len <= NFC_TAG_NDEF_BUILD_MAX);
        CHECK(buf[1] == len - 3 && buf[4] == len - 7 && buf[len - 1] == 0xFE);
        CHECK(nfc_tag_ndef_bytes_needed(buf, len) <= len);
        char link[NFC_TAG_LINK_MAX], url[SPOTIFY_REF_URL_MAX];
        CHECK_ERR(nfc_tag_parse_ndef_ex(buf, len, &p, link, sizeof link), NFC_TAG_OK);
        CHECK(spotify_ref_equal(&s, &p));
        spotify_ref_to_url(&s, url, sizeof url);
        CHECK(strcmp(link, url) == 0);
    }
}

/* ---- NDEF images ----------------------------------------------------------------------- */

static void test_ndef_images(void)
{
    uint8_t mem[1200], msg[1100], pay[1000];
    char link[NFC_TAG_LINK_MAX];
    spotify_ref_t r;
    size_t n, ml, pl;

    /* NTAG215 as written by "NFC Tools": URL record, prefix 0x04 "https://". */
    n = hexb("03 3C D1 01 38 55 04 6F 70 65 6E 2E 73 70 6F 74"
             "69 66 79 2E 63 6F 6D 2F 70 6C 61 79 6C 69 73 74"
             "2F 33 37 69 39 64 51 5A 46 31 44 58 63 42 57 49"
             "47 6F 59 42 4D 35 4D 3F 73 69 3D 61 62 63 FE 00", mem);
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_OK);
    CHECK(ref_is(&r, SPOTIFY_REF_PLAYLIST, ID_PL));
    CHECK(strcmp(link, "https://open.spotify.com/playlist/" ID_PL "?si=abc") == 0);
    CHECK(nfc_tag_parse_ndef(mem, n, &r));
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_OK);
    /* Link copy is truncated to the buffer. */
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, 9), NFC_TAG_OK);
    CHECK(strcmp(link, "https://") == 0);
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, 1), NFC_TAG_OK);
    CHECK(link[0] == '\0');
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, NULL, 50), NFC_TAG_OK);
    /* The rest of a 504-byte data area after FE does not matter. */
    memset(mem + n, 0xA5, 504 - n);
    mem[n - 1] = 0xFE;
    CHECK_ERR(parse(mem, 504, &r), NFC_TAG_OK);

    /* NTAG213 with a Lock Control TLV in front. */
    pl = uri_payload(pay, 0x04, "open.spotify.com/album/" ID_AL);
    ml = rec(msg, F_MB | F_ME, 1, "U", pay, pl);
    n = tlv_ex(mem, "01 03 A0 0C 34", msg, ml, 0);
    CHECK(mem[5] == 0x03);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);
    CHECK(ref_is(&r, SPOTIFY_REF_ALBUM, ID_AL));
    /* Memory control, proprietary and NULL TLVs are skipped as well. */
    n = tlv_ex(mem, "00 00 02 03 3F 04 13 FD 02 AB CD 00", msg, ml, 0);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);
    /* 3-byte length form for a short message (allowed). */
    n = tlv_ex(mem, NULL, msg, ml, 1);
    CHECK(mem[1] == 0xFF && mem[2] == 0x00);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);

    /* All URI identifier codes: only "" and http(s):// give a Spotify link. */
    for (int code = 0; code <= 0x23; code++) {
        n = uri_image(mem, (uint8_t)code, "open.spotify.com/track/" ID_TR);
        nfc_tag_err_t e = nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link);
        if (code == 0x00 || code == 0x03 || code == 0x04) {
            CHECK_ERR(e, NFC_TAG_OK);
        } else if (code == 0x01 || code == 0x02) {
            CHECK_ERR(e, NFC_TAG_ERR_UNSUPPORTED_TYPE);   /* host www.open.spotify.com */
        } else {
            CHECK_ERR(e, NFC_TAG_ERR_NOT_SPOTIFY);
        }
    }
    n = uri_image(mem, 0x05, "+4930123456");
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_NOT_SPOTIFY);
    CHECK(strcmp(link, "tel:+4930123456") == 0);
    n = uri_image(mem, 0x1E, "x");
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_NOT_SPOTIFY);
    CHECK(strcmp(link, "urn:epc:id:x") == 0);
    n = uri_image(mem, 0x23, "x");
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_NOT_SPOTIFY);
    CHECK(strcmp(link, "urn:nfc:x") == 0);
    n = uri_image(mem, 0x00, "spotify:artist:" ID_AR);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);
    CHECK(ref_is(&r, SPOTIFY_REF_ARTIST, ID_AR));
    n = uri_image(mem, 0x24, "open.spotify.com/track/" ID_TR);     /* RFU code */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    n = uri_image(mem, 0x04, "spotify.link/AbCdEf");
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_SHORT_LINK);
    CHECK(strcmp(link, "https://spotify.link/AbCdEf") == 0);
    n = uri_image(mem, 0x04, "open.spotify.com/user/someone");
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_UNSUPPORTED_TYPE);
    n = uri_image(mem, 0x04, "open.spotify.com/track/abc");
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_BAD_ID);
    n = uri_image(mem, 0x00, "   ");
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_LINK);

    /* Long record (SR = 0) with a long query; TLV needs the 3-byte length. */
    {
        char rest[400] = "open.spotify.com/playlist/" ID_PL "?si=";
        size_t k = strlen(rest);
        memset(rest + k, 'z', 300);
        rest[k + 300] = '\0';
        n = uri_image(mem, 0x04, rest);
        CHECK(mem[1] == 0xFF && (mem[4] & F_SR) == 0);
        CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_OK);
        CHECK(strlen(link) == NFC_TAG_LINK_MAX - 1);
        CHECK(nfc_tag_ndef_bytes_needed(mem, 16) == 4 + (size_t)(mem[2] << 8 | mem[3]));
    }
    /* Long record form also for a short payload. */
    pl = uri_payload(pay, 0x04, "open.spotify.com/show/" ID_SH);
    ml = rec_ex(msg, F_MB | F_ME, 1, "U", 1, "", 0, pay, pl, 1);
    n = tlv(mem, msg, ml);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);
    CHECK(ref_is(&r, SPOTIFY_REF_SHOW, ID_SH));

    /* Record with an ID field (IL). */
    ml = rec_ex(msg, F_MB | F_ME, 1, "U", 1, "id1", 3, pay, pl, 0);
    n = tlv(mem, msg, ml);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);

    /* Text record containing a link. */
    pl = text_payload(pay, "de", "H\xC3\xB6r mal: https://open.spotify.com/intl-de/album/" ID_AL "?si=x. Viel Spa\xC3\x9F!");
    ml = rec(msg, F_MB | F_ME, 1, "T", pay, pl);
    n = tlv(mem, msg, ml);
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_OK);
    CHECK(ref_is(&r, SPOTIFY_REF_ALBUM, ID_AL));
    CHECK(strcmp(link, "https://open.spotify.com/intl-de/album/" ID_AL "?si=x") == 0);
    /* Text variants. */
    static const struct { const char *text; nfc_tag_err_t err; const char *link; } texts[] = {
        { "spotify:track:" ID_TR, NFC_TAG_OK, "spotify:track:" ID_TR },
        { "Spotify: spotify:track:" ID_TR, NFC_TAG_OK, "spotify:track:" ID_TR },
        { "Spotify:https://open.spotify.com/track/" ID_TR, NFC_TAG_OK, "https://open.spotify.com/track/" ID_TR },
        { "Link:open.spotify.com/track/" ID_TR "!", NFC_TAG_OK, "open.spotify.com/track/" ID_TR },
        { "(https://open.spotify.com/track/" ID_TR ")", NFC_TAG_OK, "https://open.spotify.com/track/" ID_TR },
        { "<open.spotify.com/track/" ID_TR ">", NFC_TAG_OK, "open.spotify.com/track/" ID_TR },
        { "see www.example.com then https://open.spotify.com/track/" ID_TR, NFC_TAG_OK, "https://open.spotify.com/track/" ID_TR },
        { "https://spoti.fi/abc or https://example.org", NFC_TAG_ERR_SHORT_LINK, "https://spoti.fi/abc" },
        { "https://example.org, then https://spoti.fi/abc", NFC_TAG_ERR_SHORT_LINK, "https://spoti.fi/abc" },
        { "visit www.example.com", NFC_TAG_ERR_NOT_SPOTIFY, "www.example.com" },
        { "Hallo Welt, Spotify: toll", NFC_TAG_ERR_NO_LINK, "" },
        { "", NFC_TAG_ERR_NO_LINK, "" },
    };
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        pl = text_payload(pay, "en", texts[i].text);
        ml = rec(msg, F_MB | F_ME, 1, "T", pay, pl);
        n = tlv(mem, msg, ml);
        strcpy(link, "junk");
        nfc_tag_err_t e = nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link);
        if (e != texts[i].err || strcmp(link, texts[i].link) != 0) {
            fprintf(stderr, "text \"%s\": %s \"%s\"\n", texts[i].text, nfc_tag_err_str(e), link);
        }
        CHECK(e == texts[i].err);
        CHECK(strcmp(link, texts[i].link) == 0);
    }
    /* UTF-16 text (BE without BOM, LE with BOM). */
    {
        const char *t = "Ö spotify:playlist:" ID_PL ".";
        size_t tl = strlen(t);
        for (int le = 0; le < 2; le++) {
            size_t k = 0;
            pay[k++] = 0x80 | 2;
            pay[k++] = 'd';
            pay[k++] = 'e';
            if (le) {
                pay[k++] = 0xFF;
                pay[k++] = 0xFE;
            }
            for (size_t i = 0; i < tl; i++) {
                uint8_t c = (uint8_t)t[i];
                uint16_t u = c == 0xC3 ? 0x00D6 : c;   /* the UTF-8 "Ö" -> U+00D6 */
                if (c == 0x96) {
                    continue;
                }
                pay[k++] = (uint8_t)(le ? u : u >> 8);
                pay[k++] = (uint8_t)(le ? u >> 8 : u);
            }
            ml = rec(msg, F_MB | F_ME, 1, "T", pay, k);
            n = tlv(mem, msg, ml);
            CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_OK);
            CHECK(ref_is(&r, SPOTIFY_REF_PLAYLIST, ID_PL));
            CHECK(strcmp(link, "spotify:playlist:" ID_PL) == 0);
        }
    }
    /* Text record with a language length past the payload. */
    pay[0] = 0x3F;
    ml = rec(msg, F_MB | F_ME, 1, "T", pay, 3);
    n = tlv(mem, msg, ml);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);

    /* Smart poster: title Text record + URI record + action. */
    {
        uint8_t sp[300];
        size_t sl = 0;
        pl = text_payload(pay, "en", "https://example.com/title-is-ignored");
        sl += rec(sp + sl, F_MB, 1, "T", pay, pl);
        pl = uri_payload(pay, 0x04, "open.spotify.com/track/" ID_TR);
        sl += rec(sp + sl, 0, 1, "U", pay, pl);
        uint8_t act = 0;
        sl += rec(sp + sl, F_ME, 1, "act", &act, 1);
        ml = rec(msg, F_MB | F_ME, 1, "Sp", sp, sl);
        n = tlv(mem, msg, ml);
        CHECK(mem[2] == 0xD1 && mem[3] == 0x02);
        CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_OK);
        CHECK(ref_is(&r, SPOTIFY_REF_TRACK, ID_TR));
        CHECK(strcmp(link, "https://open.spotify.com/track/" ID_TR) == 0);
        /* Smart poster without URI record. */
        sl = rec(sp, F_MB | F_ME, 1, "T", pay, text_payload(pay, "en", "https://open.spotify.com/track/" ID_TR));
        ml = rec(msg, F_MB | F_ME, 1, "Sp", sp, sl);
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_LINK);
        /* Malformed nested message. */
        sp[2] = 200;
        ml = rec(msg, F_MB | F_ME, 1, "Sp", sp, sl);
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    }

    /* Absolute URI record (TNF 3, URI in the type field). */
    {
        const char *u = "https://open.spotify.com/episode/" ID_EP;
        ml = rec_ex(msg, F_MB | F_ME, 3, u, strlen(u), "", 0, pay, 0, 0);
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);
        CHECK(ref_is(&r, SPOTIFY_REF_EPISODE, ID_EP));
        ml = rec_ex(msg, F_MB | F_ME, 3, "", 0, "", 0, (const uint8_t *)u, strlen(u), 0);
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);
    }

    /* Several records: AAR + other link + Spotify link -> the Spotify one. */
    {
        const char *aar = "com.spotify.music";
        ml = rec(msg, F_MB, 4, "android.com:pkg", (const uint8_t *)aar, strlen(aar));
        pl = uri_payload(pay, 0x04, "example.com/");
        ml += rec(msg + ml, 0, 1, "U", pay, pl);
        pl = uri_payload(pay, 0x04, "open.spotify.com/artist/" ID_AR);
        ml += rec(msg + ml, 0, 1, "U", pay, pl);
        pl = uri_payload(pay, 0x04, "open.spotify.com/track/" ID_TR);
        ml += rec(msg + ml, F_ME, 1, "U", pay, pl);
        n = tlv(mem, msg, ml);
        CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_OK);
        CHECK(ref_is(&r, SPOTIFY_REF_ARTIST, ID_AR));   /* first valid link wins */
        /* Only the AAR: no link. */
        ml = rec(msg, F_MB | F_ME, 4, "android.com:pkg", (const uint8_t *)aar, strlen(aar));
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_LINK);
        /* MIME record only. */
        ml = rec(msg, F_MB | F_ME, 2, "text/plain", (const uint8_t *)"hi", 2);
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_LINK);
        /* Other link then short link: the Spotify-specific reason wins. */
        pl = uri_payload(pay, 0x04, "example.com/");
        ml = rec(msg, F_MB, 1, "U", pay, pl);
        pl = uri_payload(pay, 0x04, "spotify.link/xyz");
        ml += rec(msg + ml, F_ME, 1, "U", pay, pl);
        n = tlv(mem, msg, ml);
        CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_SHORT_LINK);
        CHECK(strcmp(link, "https://spotify.link/xyz") == 0);
        /* Two non-Spotify links: the first one is reported. */
        pl = uri_payload(pay, 0x04, "example.com/");
        ml = rec(msg, F_MB, 1, "U", pay, pl);
        pl = uri_payload(pay, 0x03, "example.org/");
        ml += rec(msg + ml, F_ME, 1, "U", pay, pl);
        n = tlv(mem, msg, ml);
        CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_NOT_SPOTIFY);
        CHECK(strcmp(link, "https://example.com/") == 0);
        /* Records after ME are ignored. */
        pl = uri_payload(pay, 0x04, "example.com/");
        ml = rec(msg, F_MB | F_ME, 1, "U", pay, pl);
        pl = uri_payload(pay, 0x04, "open.spotify.com/track/" ID_TR);
        ml += rec(msg + ml, F_ME, 1, "U", pay, pl);
        n = tlv(mem, msg, ml);
        CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NOT_SPOTIFY);
    }

    /* Chunked records. */
    pl = uri_payload(pay, 0x04, "open.spotify.com/track/" ID_TR);
    ml = rec(msg, F_MB | F_CF, 1, "U", pay, 10);
    ml += rec_ex(msg + ml, F_ME, 6, "", 0, "", 0, pay + 10, pl - 10, 0);
    n = tlv(mem, msg, ml);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_CHUNKED);
    /* A chunked record before a valid one does not hide it. */
    ml = rec(msg, F_MB | F_CF, 2, "a/b", pay, 4);
    ml += rec_ex(msg + ml, 0, 6, "", 0, "", 0, pay, 4, 0);
    ml += rec(msg + ml, F_ME, 1, "U", pay, pl);
    n = tlv(mem, msg, ml);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_OK);

    /* Malformed records. */
    n = hexb("03 05 D1 01 09 55 04 FE", mem);            /* payload past the message */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    n = hexb("03 02 C1 01 FE", mem);                      /* long header cut */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    n = hexb("03 0A C1 01 FF FF FF F0 55 04 61 62 FE", mem);  /* huge 32-bit length */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    n = hexb("03 04 D7 01 00 55 FE", mem);                /* TNF 7 reserved */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    n = hexb("03 04 D0 01 00 55 FE", mem);                /* TNF empty with a type */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);
    n = hexb("03 04 D1 01 00 55 FE", mem);                /* URI without payload */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_RECORD);

    /* Empty and missing NDEF messages. */
    n = hexb("03 00 FE 00 00 00 00 00 00 00 00 00 00 00 00 00", mem);   /* factory NTAG215 */
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, n, &r, link, sizeof link), NFC_TAG_ERR_EMPTY);
    CHECK(link[0] == '\0');
    CHECK(!nfc_tag_parse_ndef(mem, n, &r));
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_ERR_EMPTY);
    n = hexb("01 03 A0 0C 34 03 00 FE", mem);            /* factory NTAG213 */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_EMPTY);
    n = hexb("03 03 D0 00 00 FE", mem);                   /* Android "empty" */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_EMPTY);
    n = hexb("FE 00 00 00", mem);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_NDEF);
    n = hexb("01 03 A0 0C 34 FE 03 05", mem);             /* NDEF after terminator */
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_NDEF);
    n = hexb("00 00 00 00 00 00 00 00", mem);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_NO_NDEF);
    CHECK_ERR(parse(mem, 0, &r), NFC_TAG_ERR_NO_NDEF);
    n = hexb("03 FF FF FF 00", mem);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_TLV);

    /* Truncation. */
    n = uri_image(mem, 0x04, "open.spotify.com/track/" ID_TR);
    for (size_t k = 1; k < 2 + (size_t)mem[1]; k++) {
        CHECK_ERR(parse(mem, k, &r), NFC_TAG_ERR_TRUNCATED);
    }
    CHECK_ERR(parse(mem, 2 + (size_t)mem[1], &r), NFC_TAG_OK);   /* FE not needed */
    n = hexb("01 03 A0", mem);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_TRUNCATED);
    n = hexb("03 FF 01", mem);
    CHECK_ERR(parse(mem, n, &r), NFC_TAG_ERR_TRUNCATED);

    /* Arguments. */
    strcpy(link, "junk");
    CHECK_ERR(nfc_tag_parse_ndef_ex(NULL, 4, &r, link, sizeof link), NFC_TAG_ERR_ARG);
    CHECK(link[0] == '\0');
    CHECK_ERR(nfc_tag_parse_ndef_ex(mem, 4, NULL, NULL, 0), NFC_TAG_ERR_ARG);
    CHECK(!nfc_tag_parse_ndef(NULL, 0, &r));
    CHECK_ERR(nfc_tag_last_error(), NFC_TAG_ERR_ARG);
}

/* ---- Incremental reading ---------------------------------------------------------------- */

static void test_bytes_needed(void)
{
    uint8_t mem[600];
    size_t n = uri_image(mem, 0x04, "open.spotify.com/playlist/" ID_PL "?si=abc");
    size_t full = 2 + (size_t)mem[1];
    CHECK(nfc_tag_ndef_bytes_needed(mem, 0) == 1);
    CHECK(nfc_tag_ndef_bytes_needed(mem, 1) == 2);
    for (size_t k = 2; k <= n; k++) {
        CHECK(nfc_tag_ndef_bytes_needed(mem, k) == full);
    }
    /* Simulated reader: 16 bytes per READ until enough. */
    size_t have = 16, reads = 1;
    while (nfc_tag_ndef_bytes_needed(mem, have) > have) {
        have += 16;
        reads++;
    }
    CHECK(reads == 4 && have >= full);

    n = hexb("01 03 A0 0C 34 03 00 FE", mem);
    CHECK(nfc_tag_ndef_bytes_needed(mem, 1) == 2);
    CHECK(nfc_tag_ndef_bytes_needed(mem, 2) == 6);   /* skip control TLV + next T */
    CHECK(nfc_tag_ndef_bytes_needed(mem, 6) == 7);
    CHECK(nfc_tag_ndef_bytes_needed(mem, n) == 7);
    n = hexb("FE", mem);
    CHECK(nfc_tag_ndef_bytes_needed(mem, n) == 1);
    n = hexb("00 00 00 00", mem);
    CHECK(nfc_tag_ndef_bytes_needed(mem, n) == 5);   /* undecided: read on */
    n = hexb("03 FF 01", mem);
    CHECK(nfc_tag_ndef_bytes_needed(mem, n) == 4);
    n = hexb("03 FF 01 00", mem);
    CHECK(nfc_tag_ndef_bytes_needed(mem, n) == 4 + 256);
    n = hexb("03 FF FF FF", mem);
    CHECK(nfc_tag_ndef_bytes_needed(mem, n) == 4);   /* decided: TLV error */
    CHECK(nfc_tag_ndef_bytes_needed(NULL, 10) == 0);
}

/* ---- Type 2 tag helpers ------------------------------------------------------------------ */

static void test_t2t(void)
{
    size_t ds = 0;
    const uint8_t cc215[4] = { 0xE1, 0x10, 0x3E, 0x00 }, cc213[4] = { 0xE1, 0x10, 0x12, 0x00 };
    CHECK_ERR(nfc_tag_t2t_check_cc(cc215, true, &ds), NFC_TAG_OK);
    CHECK(ds == 496);
    CHECK_ERR(nfc_tag_t2t_check_cc(cc213, false, &ds), NFC_TAG_OK);
    CHECK(ds == 144);
    CHECK_ERR(nfc_tag_t2t_check_cc(cc213, true, NULL), NFC_TAG_OK);
    const uint8_t v11[4] = { 0xE1, 0x11, 0x6D, 0x00 };
    CHECK_ERR(nfc_tag_t2t_check_cc(v11, true, &ds), NFC_TAG_OK);
    CHECK(ds == 872);
    const uint8_t ro[4] = { 0xE1, 0x10, 0x3E, 0x0F };
    CHECK_ERR(nfc_tag_t2t_check_cc(ro, false, NULL), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_t2t_check_cc(ro, true, NULL), NFC_TAG_ERR_READ_ONLY);
    const uint8_t blank[4] = { 0, 0, 0, 0 }, nosize[4] = { 0xE1, 0x10, 0x00, 0x00 };
    CHECK_ERR(nfc_tag_t2t_check_cc(blank, false, NULL), NFC_TAG_ERR_NOT_FORMATTED);
    CHECK_ERR(nfc_tag_t2t_check_cc(nosize, false, NULL), NFC_TAG_ERR_NOT_FORMATTED);
    const uint8_t v2[4] = { 0xE1, 0x20, 0x3E, 0x00 }, rprop[4] = { 0xE1, 0x10, 0x3E, 0x80 };
    CHECK_ERR(nfc_tag_t2t_check_cc(v2, false, NULL), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_t2t_check_cc(rprop, false, NULL), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_t2t_check_cc(NULL, false, NULL), NFC_TAG_ERR_ARG);

    const uint8_t p2ok[4] = { 0x8C, 0x48, 0x00, 0x00 }, p2l[4] = { 0x8C, 0x48, 0x00, 0x08 };
    const uint8_t p2cc[4] = { 0x8C, 0x48, 0x08, 0x00 };
    CHECK_ERR(nfc_tag_t2t_check_static_lock(p2ok), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_t2t_check_static_lock(p2l), NFC_TAG_ERR_READ_ONLY);
    CHECK_ERR(nfc_tag_t2t_check_static_lock(p2cc), NFC_TAG_ERR_READ_ONLY);
    CHECK_ERR(nfc_tag_t2t_check_static_lock(NULL), NFC_TAG_ERR_ARG);

    /* GET_VERSION */
    nfc_tag_ntag_info_t i213, i215, i216, tmp;
    const uint8_t v213[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x0F, 0x03 };
    const uint8_t v215[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03 };
    const uint8_t v216[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x13, 0x03 };
    const uint8_t v213f[8] = { 0x00, 0x04, 0x04, 0x04, 0x01, 0x00, 0x0F, 0x03 };
    CHECK_ERR(nfc_tag_ntag_from_version(v213, &i213), NFC_TAG_OK);
    CHECK(i213.model == 213 && i213.total_pages == 45 && i213.user_bytes == 144 && i213.dyn_lock_page == 0x28);
    CHECK_ERR(nfc_tag_ntag_from_version(v215, &i215), NFC_TAG_OK);
    CHECK(i215.model == 215 && i215.total_pages == 135 && i215.user_bytes == 504 && i215.dyn_lock_page == 0x82);
    CHECK_ERR(nfc_tag_ntag_from_version(v216, &i216), NFC_TAG_OK);
    CHECK(i216.model == 216 && i216.total_pages == 231 && i216.user_bytes == 888 && i216.dyn_lock_page == 0xE2);
    CHECK_ERR(nfc_tag_ntag_from_version(v213f, &tmp), NFC_TAG_OK);
    CHECK((size_t)(i215.dyn_lock_page - 4) * 4 == i215.user_bytes);
    const uint8_t v210[8] = { 0x00, 0x04, 0x04, 0x01, 0x01, 0x00, 0x0B, 0x03 };     /* NTAG210 */
    const uint8_t ul[8] = { 0x00, 0x04, 0x03, 0x01, 0x01, 0x00, 0x0B, 0x03 };      /* Ultralight EV1 */
    const uint8_t i2c[8] = { 0x00, 0x04, 0x04, 0x05, 0x02, 0x01, 0x13, 0x03 };     /* NTAG I2C 1k */
    const uint8_t other[8] = { 0x00, 0x05, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03 };
    const uint8_t size12[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x12, 0x03 };
    CHECK_ERR(nfc_tag_ntag_from_version(v210, &tmp), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_ntag_from_version(ul, &tmp), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_ntag_from_version(i2c, &tmp), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_ntag_from_version(other, &tmp), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_ntag_from_version(size12, &tmp), NFC_TAG_ERR_UNSUPPORTED);
    CHECK_ERR(nfc_tag_ntag_from_version(NULL, &tmp), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_ntag_from_version(v215, NULL), NFC_TAG_ERR_ARG);

    /* Configuration pages (factory: dyn lock 00 00 00, AUTH0 FF). */
    uint8_t cfg[16];
    hexb("00 00 00 BD 04 00 00 FF 00 05 00 00 00 00 00 00", cfg);
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 19), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 0x81), NFC_TAG_OK);
    cfg[7] = 0x10;                                   /* AUTH0 = page 16 */
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 15), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 16), NFC_TAG_ERR_PASSWORD);
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 19), NFC_TAG_ERR_PASSWORD);
    cfg[7] = 0x04;
    CHECK_ERR(nfc_tag_ntag_check_config(&i213, cfg, 4), NFC_TAG_ERR_PASSWORD);
    cfg[7] = 0xFF;
    cfg[2] = 0x01;                                   /* one dynamic lock bit */
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 19), NFC_TAG_ERR_READ_ONLY);
    cfg[2] = 0;
    cfg[0] = 0x80;
    CHECK_ERR(nfc_tag_ntag_check_config(&i213, cfg, 19), NFC_TAG_ERR_READ_ONLY);
    cfg[0] = 0;
    CHECK_ERR(nfc_tag_ntag_check_config(NULL, cfg, 19), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, NULL, 19), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_ntag_check_config(&i215, cfg, 3), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_ntag_check_config(&i213, cfg, 0x28), NFC_TAG_ERR_ARG);
}

/* Checks the invariants of a successful plan and that the image parses back. */
static void check_plan(const nfc_tag_t2t_plan_t *p, const uint8_t *cur, const spotify_ref_t *ref)
{
    CHECK(p->end % 4 == 0 && p->end <= NFC_TAG_T2T_PLAN_MAX);
    CHECK(p->img[p->ndef_offset] == 0x03 && p->len_offset == p->ndef_offset + 1);
    CHECK(p->first_page == 4 + p->ndef_offset / 4);
    CHECK(p->len_page == 4 + p->len_offset / 4);
    CHECK(p->last_page == 4 + p->end / 4 - 1);
    CHECK(p->first_page <= p->len_page && p->len_page <= p->last_page);
    CHECK(memcmp(p->img, cur, p->ndef_offset) == 0);
    size_t stop = p->len_offset + 1 + p->img[p->len_offset];
    CHECK(p->img[stop] == 0xFE);
    for (size_t i = stop + 1; i < p->end; i++) {
        CHECK(p->img[i] == 0);
    }
    spotify_ref_t r;
    CHECK_ERR(parse(p->img, p->end, &r), NFC_TAG_OK);
    CHECK(spotify_ref_equal(&r, ref));
    /* Step 1 state (length byte 0) reads as an empty message. */
    uint8_t step1[NFC_TAG_T2T_PLAN_MAX];
    memcpy(step1, p->img, p->end);
    step1[p->len_offset] = 0;
    CHECK_ERR(parse(step1, p->end, &r), NFC_TAG_ERR_EMPTY);
}

static void test_plan_write(void)
{
    spotify_ref_t pl = { SPOTIFY_REF_PLAYLIST, ID_PL }, tr = { SPOTIFY_REF_TRACK, ID_TR };
    nfc_tag_t2t_plan_t p;
    uint8_t cur[64];
    size_t n;

    /* Factory NTAG215: 03 00 FE. */
    n = hexb("03 00 FE 00 00 00 00 00 00 00 00 00 00 00 00 00"
             "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_OK);
    CHECK(p.ndef_offset == 0 && p.len_offset == 1 && p.end == 56);
    CHECK(p.first_page == 4 && p.len_page == 4 && p.last_page == 17);
    uint8_t built[64];
    CHECK(nfc_tag_build_ndef(&pl, built, sizeof built) == 56);
    CHECK(memcmp(p.img, built, 56) == 0);
    check_plan(&p, cur, &pl);
    /* Track: 53 bytes + 3 bytes of padding. */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &tr, &p), NFC_TAG_OK);
    CHECK(p.end == 56 && p.img[52] == 0xFE && p.img[53] == 0 && p.last_page == 17);
    check_plan(&p, cur, &tr);

    /* Factory NTAG213 with the Lock Control TLV: kept, NDEF TLV at offset 5. */
    n = hexb("01 03 A0 0C 34 03 00 FE 00 00 00 00 00 00 00 00"
             "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 144, &pl, &p), NFC_TAG_OK);
    CHECK(p.ndef_offset == 5 && p.len_offset == 6 && p.end == 64);
    CHECK(p.first_page == 5 && p.len_page == 5 && p.last_page == 19);
    CHECK(memcmp(p.img, "\x01\x03\xA0\x0C\x34\x03\x35\xD1", 8) == 0);
    check_plan(&p, cur, &pl);
    /* Length byte in the next page than the T byte. */
    n = hexb("00 00 01 03 A0 0C 34 03 00 FE 00 00 00 00 00 00", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 144, &pl, &p), NFC_TAG_OK);
    CHECK(p.ndef_offset == 7 && p.first_page == 5 && p.len_page == 6 && p.end == 64);
    check_plan(&p, cur, &pl);
    /* Only the control TLVs need to be known. */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, 7, 144, &pl, &p), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, 6, 144, &pl, &p), NFC_TAG_ERR_TRUNCATED);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, 3, 144, &pl, &p), NFC_TAG_ERR_TRUNCATED);
    /* Existing content (an old link, proprietary TLV, zeros, garbage) is replaced. */
    n = uri_image(cur, 0x04, "open.spotify.com/track/" ID_TR);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, 32, 496, &pl, &p), NFC_TAG_OK);
    CHECK(p.ndef_offset == 0);
    check_plan(&p, cur, &pl);
    memset(cur, 0, 32);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, 32, 496, &pl, &p), NFC_TAG_OK);
    CHECK(p.ndef_offset == 0);
    hexb("FD 02 12 34 FE", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, 32, 496, &pl, &p), NFC_TAG_OK);
    CHECK(p.ndef_offset == 0);
    CHECK_ERR(nfc_tag_t2t_plan_write(NULL, 0, 496, &pl, &p), NFC_TAG_OK);

    /* Space: exactly fits, one byte short. */
    n = hexb("03 00 FE 00", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 56, &pl, &p), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 55, &pl, &p), NFC_TAG_ERR_NO_SPACE);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 48, &pl, &p), NFC_TAG_ERR_NO_SPACE);
    n = hexb("01 03 A0 0C 34 03 00 FE", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 61, &pl, &p), NFC_TAG_OK);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 60, &pl, &p), NFC_TAG_ERR_NO_SPACE);
    /* Too many control TLVs for the plan buffer. */
    {
        uint8_t many[100];
        for (int i = 0; i < 20; i++) {
            hexb("02 03 F0 01 04", many + 5 * i);    /* reserves bytes far away */
        }
        CHECK_ERR(nfc_tag_t2t_plan_write(many, 100, 496, &pl, &p), NFC_TAG_ERR_NO_SPACE);
        CHECK_ERR(nfc_tag_t2t_plan_write(many, 50, 496, &pl, &p), NFC_TAG_ERR_NO_SPACE);
        CHECK(p.end == 0);
        CHECK_ERR(nfc_tag_t2t_plan_write(many, 30, 496, &pl, &p), NFC_TAG_OK);
        CHECK(p.ndef_offset == 30);
        check_plan(&p, many, &pl);
    }

    /* Malformed control TLVs or reserved areas inside the written range. */
    n = hexb("01 04 A0 0C 34 00 03 00 FE", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 144, &pl, &p), NFC_TAG_ERR_UNSUPPORTED);
    n = hexb("01 FF 00 03 A0 0C 34 FE", cur);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 144, &pl, &p), NFC_TAG_ERR_UNSUPPORTED);
    n = hexb("02 03 C0 04 22 03 00 FE", cur);       /* page 12 = bytes 48..51 = offset 32 */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_ERR_UNSUPPORTED);
    n = hexb("02 03 F0 04 22 03 00 FE", cur);       /* page 15 = byte 60 = offset 44 */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_ERR_UNSUPPORTED);
    n = hexb("02 03 F0 04 34 03 00 FE", cur);       /* 15 * 16 = byte 240: beyond the write */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_OK);
    n = hexb("01 03 50 10 22 03 00 FE", cur);       /* lock bytes 20..21 = page 5, written */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_ERR_UNSUPPORTED);
    n = hexb("01 03 40 10 22 03 00 FE", cur);       /* page 4 holds only kept bytes */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_OK);
    CHECK(p.first_page == 5);
    n = hexb("01 03 30 10 22 03 00 FE", cur);       /* bytes 12..13: page 3, untouched */
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, &p), NFC_TAG_OK);

    /* Arguments. */
    spotify_ref_t bad = pl;
    bad.id[0] = '!';
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &bad, &p), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, NULL, &p), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_t2t_plan_write(cur, n, 496, &pl, NULL), NFC_TAG_ERR_ARG);
    CHECK_ERR(nfc_tag_t2t_plan_write(NULL, 4, 496, &pl, &p), NFC_TAG_ERR_ARG);
}

/* ---- Round trips -------------------------------------------------------------------------- */

static uint32_t s_rng = 0x2545F491u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void random_ref(spotify_ref_t *r)
{
    static const char b62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    r->type = (spotify_ref_type_t)(rnd() % 6);
    for (int i = 0; i < 22; i++) {
        r->id[i] = b62[rnd() % 62];
    }
    r->id[22] = '\0';
}

static void test_roundtrip(void)
{
    for (int i = 0; i < 2000; i++) {
        spotify_ref_t r, p;
        random_ref(&r);
        char uri[SPOTIFY_REF_URI_MAX], url[SPOTIFY_REF_URL_MAX];
        uint8_t mem[NFC_TAG_NDEF_BUILD_MAX];
        spotify_ref_to_uri(&r, uri, sizeof uri);
        spotify_ref_to_url(&r, url, sizeof url);
        CHECK(nfc_tag_parse_link(uri, &p) && spotify_ref_equal(&r, &p));
        CHECK(nfc_tag_parse_link(url, &p) && spotify_ref_equal(&r, &p));
        size_t n = nfc_tag_build_ndef(&r, mem, sizeof mem);
        CHECK(n > 0 && nfc_tag_parse_ndef(mem, n, &p) && spotify_ref_equal(&r, &p));
        nfc_tag_t2t_plan_t plan;
        uint8_t cur[8] = { 0x01, 0x03, 0xA0, 0x0C, 0x34, 0x03, 0x00, 0xFE };
        CHECK_ERR(nfc_tag_t2t_plan_write(cur, sizeof cur, 144, &r, &plan), NFC_TAG_OK);
        check_plan(&plan, cur, &r);
    }
}

/* ---- Fuzzing ------------------------------------------------------------------------------- */

/* Valid images that the mutator starts from. */
static size_t seed_image(int k, uint8_t *mem)
{
    uint8_t msg[400], pay[300], sp[300];
    size_t ml, pl, sl;
    switch (k % 6) {
    case 0:
        return uri_image(mem, 0x04, "open.spotify.com/playlist/" ID_PL "?si=abc");
    case 1:
        pl = text_payload(pay, "de", "Hör mal: https://open.spotify.com/intl-de/album/" ID_AL);
        ml = rec(msg, F_MB | F_ME, 1, "T", pay, pl);
        return tlv_ex(mem, "01 03 A0 0C 34", msg, ml, 0);
    case 2:
        pl = uri_payload(pay, 0x04, "open.spotify.com/track/" ID_TR);
        sl = rec(sp, F_MB, 1, "U", pay, pl);
        pl = text_payload(pay, "en", "title");
        sl += rec(sp + sl, F_ME, 1, "T", pay, pl);
        ml = rec(msg, F_MB | F_ME, 1, "Sp", sp, sl);
        return tlv(mem, msg, ml);
    case 3:
        ml = rec(msg, F_MB, 4, "android.com:pkg", (const uint8_t *)"com.spotify.music", 17);
        pl = uri_payload(pay, 0x00, "spotify:user:x:playlist:" ID_PL);
        ml += rec_ex(msg + ml, F_ME, 1, "U", 1, "i", 1, pay, pl, 1);
        return tlv_ex(mem, NULL, msg, ml, 1);
    case 4:
        pl = text_payload(pay, "en", "a spotify:track:" ID_TR " b");
        pay[0] |= 0x80;
        ml = rec(msg, F_MB | F_ME, 1, "T", pay, pl);
        return tlv(mem, msg, ml);
    default:
        return hexb("01 03 A0 0C 34 03 00 FE 00 00 00 00 00 00 00 00", mem);
    }
}

static void fuzz_one(const uint8_t *src, size_t len)
{
    /* Exact-size heap copy so that ASan catches any read past the end. */
    uint8_t *mem = malloc(len ? len : 1);
    memcpy(mem, src, len);
    spotify_ref_t r, r2;
    char link[64];
    size_t cap = rnd() % (sizeof link + 1);
    nfc_tag_err_t e = nfc_tag_parse_ndef_ex(mem, len, &r, cap ? link : NULL, cap);
    CHECK(e <= NFC_TAG_ERR_UNSUPPORTED);
    if (cap) {
        CHECK(memchr(link, '\0', cap) != NULL);
    }
    if (e == NFC_TAG_OK) {
        CHECK(spotify_ref_is_valid(&r));
    }
    /* bytes_needed contract on a random prefix. */
    size_t k = len ? rnd() % (len + 1) : 0;
    size_t need = nfc_tag_ndef_bytes_needed(mem, k);
    nfc_tag_err_t ek = nfc_tag_parse_ndef_ex(mem, k, &r2, NULL, 0);
    if (need <= k) {
        CHECK(ek != NFC_TAG_ERR_TRUNCATED);
        CHECK(ek == e);
        if (ek == NFC_TAG_OK && e == NFC_TAG_OK) {
            CHECK(spotify_ref_equal(&r, &r2));
        }
    } else {
        CHECK(ek == NFC_TAG_ERR_TRUNCATED || ek == NFC_TAG_ERR_NO_NDEF);
    }
    /* Planner on the same bytes. */
    nfc_tag_t2t_plan_t plan;
    spotify_ref_t ref;
    random_ref(&ref);
    if (nfc_tag_t2t_plan_write(mem, len, 16 * (rnd() % 64), &ref, &plan) == NFC_TAG_OK) {
        check_plan(&plan, mem, &ref);
    }
    if (len >= 8) {
        nfc_tag_ntag_info_t info;
        size_t ds;
        nfc_tag_t2t_check_cc(mem, len & 1, &ds);
        nfc_tag_t2t_check_static_lock(mem);
        if (nfc_tag_ntag_from_version(mem, &info) == NFC_TAG_OK && len >= 16) {
            nfc_tag_ntag_check_config(&info, mem, (uint16_t)(rnd() % 300));
        }
    }
    free(mem);
}

static void test_fuzz(void)
{
    enum { ITER = 200000 };
    uint8_t buf[700];
    char text[200];
    static const char alphabet[] = "spotify:/.?#-_ htpsopenclmkrackalbumplaylistuserintl37i9dQZF1DXcBWIGoYBM5M\t\n%";
    int before = s_fail;
    for (int it = 0; it < ITER; it++) {
        size_t len;
        switch (it % 4) {
        case 0:                                    /* random bytes */
            len = rnd() % 300;
            for (size_t i = 0; i < len; i++) {
                buf[i] = (uint8_t)rnd();
            }
            if (len && rnd() % 2) {
                buf[0] = 0x03;                     /* steer into the NDEF path */
            }
            break;
        default: {                                 /* mutated valid image */
            len = seed_image((int)(rnd() % 6), buf);
            int flips = 1 + (int)(rnd() % 4);
            for (int f = 0; f < flips; f++) {
                size_t pos = rnd() % len;
                buf[pos] = (rnd() % 3) ? (uint8_t)rnd() : (uint8_t)(buf[pos] ^ (1u << (rnd() % 8)));
            }
            if (rnd() % 4 == 0) {
                len = rnd() % (len + 1);
            }
            break;
        }
        }
        fuzz_one(buf, len);

        /* Link parser with link-like strings and raw bytes. */
        size_t tl = rnd() % (sizeof text - 1);
        for (size_t i = 0; i < tl; i++) {
            text[i] = (rnd() % 8) ? alphabet[rnd() % (sizeof alphabet - 1)] : (char)(1 + rnd() % 255);
        }
        text[tl] = '\0';
        if (it % 3 == 0) {
            static const char *heads[] = { "https://open.spotify.com/", "spotify:", "open.spotify.com/intl-de/" };
            const char *h = heads[rnd() % 3];
            size_t hl = strlen(h);
            if (hl + tl < sizeof text) {
                memmove(text + hl, text, tl + 1);
                memcpy(text, h, hl);
            }
        }
        spotify_ref_t r;
        if (nfc_tag_parse_link_ex(text, &r) == NFC_TAG_OK) {
            CHECK(spotify_ref_is_valid(&r));
        }
        if (s_fail > before + 20) {
            fprintf(stderr, "fuzz: too many failures, stopping at iteration %d\n", it);
            break;
        }
    }
}

int main(void)
{
    test_err_str();
    test_ref_helpers();
    test_parse_link();
    test_build();
    test_ndef_images();
    test_bytes_needed();
    test_t2t();
    test_plan_write();
    test_roundtrip();
    test_fuzz();
    if (s_fail) {
        printf("nfc_tag host tests: %d of %d checks FAILED\n", s_fail, s_checks);
        return 1;
    }
    printf("nfc_tag host tests: all %d checks passed\n", s_checks);
    return 0;
}
