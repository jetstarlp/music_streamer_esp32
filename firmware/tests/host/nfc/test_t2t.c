/*
 * Host tests for components/nfc/t2t.c: read/write procedures against a
 * simulated NTAG213/215/216 (memory, OTP CC, lock bits, AUTH0, roll-over,
 * tag loss after N commands, silent corruption).
 * Run: firmware/tools/host_test_nfc.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t2t.h"

static int s_checks, s_fail;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        s_checks++;                                                                  \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            s_fail++;                                                                \
        }                                                                            \
    } while (0)

#define ID_PL "37i9dQZF1DXcBWIGoYBM5M"
#define ID_TR "4uLU6hMCjMI75M1A2tKUQC"

/* ---- simulated NTAG21x ----------------------------------------------------------- */

typedef struct {
    uint8_t  mem[231 * 4];
    uint16_t pages;
    uint16_t dyn_lock_page;
    uint8_t  ver[8];
    bool     answers_version;
    int      ops_left;        /* < 0: unlimited; commands until the tag "moves away" */
    bool     reader_dead;
    int      corrupt_page;    /* a write to this page stores a wrong byte (-1: none) */
    unsigned reads, writes;
} sim_t;

static uint8_t *pg(sim_t *t, uint16_t p)
{
    return t->mem + (size_t)p * 4;
}

static void sim_init(sim_t *t, uint16_t model, bool formatted)
{
    memset(t, 0, sizeof *t);
    t->ops_left = -1;
    t->corrupt_page = -1;
    t->answers_version = true;
    uint8_t size_byte = 0, prod = 0;
    switch (model) {
    case 213: t->pages = 45;  t->dyn_lock_page = 0x28; size_byte = 0x12; prod = 0x0F; break;
    case 215: t->pages = 135; t->dyn_lock_page = 0x82; size_byte = 0x3E; prod = 0x11; break;
    default:  t->pages = 231; t->dyn_lock_page = 0xE2; size_byte = 0x6D; prod = 0x13; break;
    }
    const uint8_t ver[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, prod, 0x03 };
    memcpy(t->ver, ver, 8);
    const uint8_t uid[9] = { 0x04, 0x68, 0x95, 0x7D, 0x71, 0xFA, 0x5C, 0x64, 0x00 };
    memcpy(pg(t, 0), uid, 8);
    pg(t, 2)[0] = 0x00;
    pg(t, 2)[1] = 0x48;
    if (formatted) {
        const uint8_t cc[4] = { 0xE1, 0x10, size_byte, 0x00 };
        memcpy(pg(t, 3), cc, 4);
        if (model == 213) {
            const uint8_t d[8] = { 0x01, 0x03, 0xA0, 0x0C, 0x34, 0x03, 0x00, 0xFE };
            memcpy(pg(t, 4), d, 8);
        } else {
            const uint8_t d[4] = { 0x03, 0x00, 0xFE, 0x00 };
            memcpy(pg(t, 4), d, 4);
        }
    }
    pg(t, t->dyn_lock_page)[3] = 0xBD;          /* RFUI */
    pg(t, t->dyn_lock_page + 1)[0] = 0x04;      /* CFG0: MIRROR */
    pg(t, t->dyn_lock_page + 1)[3] = 0xFF;      /* AUTH0: no protection */
    pg(t, t->dyn_lock_page + 2)[0] = 0x00;      /* CFG1 */
}

static bool sim_op(sim_t *t)
{
    if (t->ops_left == 0) {
        return false;
    }
    if (t->ops_left > 0) {
        t->ops_left--;
    }
    return true;
}

static t2t_io_status_t sim_read16(void *ctx, uint16_t page, uint8_t out[16])
{
    sim_t *t = ctx;
    if (t->reader_dead) {
        return T2T_IO_READER;
    }
    if (!sim_op(t) || page >= t->pages) {
        return T2T_IO_TAG;
    }
    t->reads++;
    for (int i = 0; i < 16; i++) {
        uint16_t p = (uint16_t)((page + i / 4) % t->pages);      /* roll-over */
        bool pwd = p == t->dyn_lock_page + 3 || p == t->dyn_lock_page + 4;
        out[i] = pwd ? 0x00 : pg(t, p)[i % 4];                   /* PWD/PACK read as 0 */
    }
    return T2T_IO_OK;
}

static t2t_io_status_t sim_write4(void *ctx, uint16_t page, const uint8_t d[4])
{
    sim_t *t = ctx;
    if (t->reader_dead) {
        return T2T_IO_READER;
    }
    if (!sim_op(t) || page < 2 || page >= t->pages) {
        return T2T_IO_TAG;
    }
    uint8_t auth0 = pg(t, t->dyn_lock_page + 1)[3];
    const uint8_t *l = pg(t, 2);
    const uint8_t *dl = pg(t, t->dyn_lock_page);
    if (page >= auth0) {
        return T2T_IO_TAG;                                       /* NAK: needs PWD_AUTH */
    }
    if (page >= 3 && page < 16 && (l[2] | l[3])) {
        return T2T_IO_TAG;                                       /* static lock (coarse) */
    }
    if (page >= 16 && page < t->dyn_lock_page && (dl[0] | dl[1] | dl[2])) {
        return T2T_IO_TAG;                                       /* dynamic lock (coarse) */
    }
    t->writes++;
    if (page == 2) {
        return T2T_IO_TAG;                                       /* never written by t2t.c */
    }
    if (page == 3) {
        for (int i = 0; i < 4; i++) {
            pg(t, 3)[i] |= d[i];                                 /* OTP */
        }
        return T2T_IO_OK;
    }
    memcpy(pg(t, page), d, 4);
    if (page == t->corrupt_page) {
        pg(t, page)[2] ^= 0x01;
    }
    return T2T_IO_OK;
}

static t2t_io_status_t sim_version(void *ctx, uint8_t out[8])
{
    sim_t *t = ctx;
    if (t->reader_dead) {
        return T2T_IO_READER;
    }
    if (!sim_op(t) || !t->answers_version) {
        return T2T_IO_TAG;
    }
    memcpy(out, t->ver, 8);
    return T2T_IO_OK;
}

static t2t_io_t io_for(sim_t *t)
{
    t2t_io_t io = { sim_read16, sim_write4, sim_version, t };
    return io;
}

static spotify_ref_t ref_of(spotify_ref_type_t type, const char *id)
{
    spotify_ref_t r;
    memset(&r, 0, sizeof r);
    r.type = type;
    snprintf(r.id, sizeof r.id, "%s", id);
    return r;
}

/* Reads the tag; returns the result and *ref. */
static t2t_result_t read_tag(sim_t *t, spotify_ref_t *ref, t2t_outcome_t *o)
{
    uint8_t buf[1024];
    char link[NFC_TAG_LINK_MAX];
    t2t_io_t io = io_for(t);
    return t2t_read_ref(&io, buf, sizeof buf, ref, link, sizeof link, o);
}

/* ---- tests ------------------------------------------------------------------------- */

static void test_write_read(uint16_t model)
{
    sim_t t;
    sim_init(&t, model, true);
    spotify_ref_t ref = ref_of(SPOTIFY_REF_PLAYLIST, ID_PL), got;
    t2t_outcome_t o;

    CHECK(read_tag(&t, &got, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_EMPTY);

    t2t_io_t io = io_for(&t);
    uint8_t before[16];
    memcpy(before, t.mem, 16);
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_OK);
    CHECK(o.model == model && !o.cc_written);
    CHECK(memcmp(before, t.mem, 16) == 0);                   /* pages 0..3 untouched */
    if (model == 213) {
        CHECK(memcmp(pg(&t, 4), "\x01\x03\xA0\x0C\x34\x03", 6) == 0);   /* lock control TLV kept */
    } else {
        CHECK(pg(&t, 4)[0] == 0x03);
    }
    /* write count: pages of the plan + the length page once more */
    unsigned writes = o.writes;
    CHECK(writes >= 14 && writes <= 17);

    memset(&got, 0, sizeof got);
    CHECK(read_tag(&t, &got, &o) == T2T_OK);
    CHECK(spotify_ref_equal(&got, &ref));
    CHECK(o.reads >= 2 && o.reads <= 5);                     /* incremental, not the whole tag */

    /* overwrite with another reference */
    spotify_ref_t ref2 = ref_of(SPOTIFY_REF_TRACK, ID_TR);
    CHECK(t2t_write_ref(&io, &ref2, &o) == T2T_OK);
    CHECK(read_tag(&t, &got, &o) == T2T_OK && spotify_ref_equal(&got, &ref2));
}

static void test_unformatted(uint16_t model, const uint8_t want_cc[4])
{
    sim_t t;
    sim_init(&t, model, false);
    spotify_ref_t ref = ref_of(SPOTIFY_REF_ALBUM, ID_PL), got;
    t2t_outcome_t o;
    CHECK(read_tag(&t, &got, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_NOT_FORMATTED);
    t2t_io_t io = io_for(&t);
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_OK);
    CHECK(o.cc_written);
    CHECK(memcmp(pg(&t, 3), want_cc, 4) == 0);
    CHECK(read_tag(&t, &got, &o) == T2T_OK && spotify_ref_equal(&got, &ref));

    uint8_t cc[4];
    CHECK(t2t_ntag_default_cc(model, cc) && memcmp(cc, want_cc, 4) == 0);
    CHECK(!t2t_ntag_default_cc(210, cc));
}

static void test_protection(void)
{
    spotify_ref_t ref = ref_of(SPOTIFY_REF_PLAYLIST, ID_PL);
    t2t_outcome_t o;
    sim_t t;
    t2t_io_t io = io_for(&t);

    sim_init(&t, 215, true);
    pg(&t, 2)[2] = 0x08;                                     /* static lock bit */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_READ_ONLY && o.writes == 0);

    sim_init(&t, 215, true);
    pg(&t, t.dyn_lock_page)[0] = 0x01;                       /* dynamic lock bit */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_READ_ONLY && o.writes == 0);

    sim_init(&t, 215, true);
    pg(&t, t.dyn_lock_page + 1)[3] = 0x04;                   /* AUTH0 = 4: password */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_PASSWORD && o.writes == 0);

    sim_init(&t, 215, true);
    pg(&t, t.dyn_lock_page + 1)[3] = 0x10;                   /* AUTH0 = 16: link needs pages 4..17 */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_PASSWORD && o.writes == 0);

    sim_init(&t, 215, true);
    pg(&t, t.dyn_lock_page + 1)[3] = 0x12;                   /* AUTH0 = 18: just enough */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_OK);

    sim_init(&t, 215, false);
    pg(&t, t.dyn_lock_page + 1)[3] = 0x03;                   /* unformatted + CC protected */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_PASSWORD && o.writes == 0);

    sim_init(&t, 215, true);
    pg(&t, 3)[3] = 0x0F;                                     /* CC: write access denied */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_READ_ONLY && o.writes == 0);

    sim_init(&t, 215, true);
    pg(&t, 3)[0] = 0xE2;                                     /* foreign CC */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_NOT_FORMATTED && o.writes == 0);

    sim_init(&t, 215, true);
    t.ver[6] = 0x0B;                                         /* not NTAG21x (Ultralight EV1 like) */
    t.ver[2] = 0x03;
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_UNSUPPORTED && o.writes == 0);

    sim_init(&t, 215, true);
    t.answers_version = false;                               /* GET_VERSION NAK */
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_IO && o.io == T2T_IO_TAG && o.writes == 0 && o.reads == 0);

    sim_init(&t, 215, true);
    t.reader_dead = true;
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_IO && o.io == T2T_IO_READER);
    spotify_ref_t got;
    CHECK(read_tag(&t, &got, &o) == T2T_ERR_IO && o.io == T2T_IO_READER);

    sim_init(&t, 215, true);
    spotify_ref_t bad = ref;
    bad.id[3] = '-';
    CHECK(t2t_write_ref(&io, &bad, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_ARG);
    CHECK(t2t_write_ref(NULL, &ref, &o) == T2T_ERR_TAG);
    CHECK(t2t_write_ref(&io, &ref, NULL) == T2T_OK);         /* outcome optional */
}

static void test_verify(void)
{
    spotify_ref_t ref = ref_of(SPOTIFY_REF_PLAYLIST, ID_PL);
    t2t_outcome_t o;
    sim_t t;
    t2t_io_t io = io_for(&t);
    sim_init(&t, 215, true);
    t.corrupt_page = 7;
    CHECK(t2t_write_ref(&io, &ref, &o) == T2T_ERR_VERIFY);
}

/* The tag leaves after k commands, for every k: afterwards it holds either
 * the old reference (nothing written yet) or the new one, or an empty NDEF
 * message - never a half-written link. */
static void test_tag_loss(void)
{
    spotify_ref_t old_ref = ref_of(SPOTIFY_REF_TRACK, ID_TR);
    spotify_ref_t new_ref = ref_of(SPOTIFY_REF_PLAYLIST, ID_PL);
    for (uint16_t model = 213; model <= 216; model += (model == 213 ? 2 : 1)) {
        sim_t t;
        t2t_io_t io = io_for(&t);
        t2t_outcome_t o;
        sim_init(&t, model, true);
        CHECK(t2t_write_ref(&io, &old_ref, &o) == T2T_OK);
        CHECK(t2t_write_ref(&io, &new_ref, &o) == T2T_OK);
        unsigned total_ops = 1 + o.reads + o.writes;         /* + GET_VERSION */
        int saw_empty = 0;
        for (unsigned k = 0; k < total_ops; k++) {
            sim_init(&t, model, true);
            CHECK(t2t_write_ref(&io, &old_ref, &o) == T2T_OK);
            t.ops_left = (int)k;
            t2t_result_t r = t2t_write_ref(&io, &new_ref, &o);
            CHECK(r == T2T_ERR_IO && o.io == T2T_IO_TAG);
            t.ops_left = -1;
            spotify_ref_t got;
            t2t_result_t rr = read_tag(&t, &got, &o);
            if (rr == T2T_OK) {
                CHECK(spotify_ref_equal(&got, &old_ref) || spotify_ref_equal(&got, &new_ref));
            } else {
                CHECK(rr == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_EMPTY);
                saw_empty++;
            }
        }
        CHECK(saw_empty > 0);
        /* enough commands: success */
        sim_init(&t, model, true);
        CHECK(t2t_write_ref(&io, &old_ref, &o) == T2T_OK);
        t.ops_left = (int)total_ops;
        CHECK(t2t_write_ref(&io, &new_ref, &o) == T2T_OK);
    }
}

/* A long Text record (3-byte TLV length, long NDEF record) with the link at
 * its end on an NTAG216: read incrementally up to the end of the TLV. */
static void test_long_read(void)
{
    sim_t t;
    sim_init(&t, 216, true);
    char text[400];
    memset(text, 'x', sizeof text);
    const char *link = " https://open.spotify.com/playlist/" ID_PL;
    size_t tl = 300;
    memcpy(text + tl - strlen(link), link, strlen(link));
    size_t payload = 3 + tl;                                 /* status, "en", text */
    size_t rec = 1 + 1 + 4 + 1 + payload;                    /* header, type len, 4-byte len, 'T' */
    uint8_t *d = pg(&t, 4);
    size_t i = 0;
    d[i++] = 0x03;
    d[i++] = 0xFF;
    d[i++] = (uint8_t)(rec >> 8);
    d[i++] = (uint8_t)rec;
    d[i++] = 0xC1;                                           /* MB ME, TNF 1, no SR */
    d[i++] = 0x01;
    d[i++] = 0;
    d[i++] = 0;
    d[i++] = (uint8_t)(payload >> 8);
    d[i++] = (uint8_t)payload;
    d[i++] = 'T';
    d[i++] = 0x02;
    d[i++] = 'e';
    d[i++] = 'n';
    memcpy(d + i, text, tl);
    i += tl;
    d[i++] = 0xFE;

    spotify_ref_t got, want = ref_of(SPOTIFY_REF_PLAYLIST, ID_PL);
    t2t_outcome_t o;
    CHECK(read_tag(&t, &got, &o) == T2T_OK && spotify_ref_equal(&got, &want));
    size_t tlv_end = 4 + rec;
    CHECK(o.bytes_read >= tlv_end && o.bytes_read < tlv_end + 16);
    CHECK(o.reads == 1 + (unsigned)((o.bytes_read - 12 + 15) / 16));

    /* buffer smaller than the TLV: no overflow, a clear error */
    uint8_t small[64];
    t2t_io_t io = io_for(&t);
    CHECK(t2t_read_ref(&io, small, sizeof small, &got, NULL, 0, &o) == T2T_ERR_TAG);
    CHECK(o.tag_err == NFC_TAG_ERR_TRUNCATED && o.bytes_read == sizeof small);

    /* CC claims a smaller data area than the TLV: reading stops there */
    pg(&t, 3)[2] = 0x06;                                     /* 48 bytes */
    CHECK(read_tag(&t, &got, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_TRUNCATED && o.bytes_read == 48);

    /* too small buffer argument */
    CHECK(t2t_read_ref(&io, small, 8, &got, NULL, 0, &o) == T2T_ERR_TAG && o.tag_err == NFC_TAG_ERR_ARG);

    /* tag leaves while reading */
    pg(&t, 3)[2] = 0x6D;
    t.ops_left = 3;
    CHECK(read_tag(&t, &got, &o) == T2T_ERR_IO && o.io == T2T_IO_TAG);
}

/* Content written by phone apps: NTAG215 with a NFC Tools style URI record. */
static void test_foreign_content(void)
{
    sim_t t;
    sim_init(&t, 215, true);
    const char *uri = "open.spotify.com/artist/0OdUWJ0sBjDrqHygGUXeCF?si=abc";
    size_t ul = strlen(uri);
    uint8_t *d = pg(&t, 4);
    size_t i = 0;
    d[i++] = 0x03;
    d[i++] = (uint8_t)(ul + 5);
    d[i++] = 0xD1;
    d[i++] = 0x01;
    d[i++] = (uint8_t)(ul + 1);
    d[i++] = 'U';
    d[i++] = 0x04;
    memcpy(d + i, uri, ul);
    i += ul;
    d[i] = 0xFE;
    spotify_ref_t got;
    t2t_outcome_t o;
    CHECK(read_tag(&t, &got, &o) == T2T_OK && got.type == SPOTIFY_REF_ARTIST);

    /* not a Spotify link: link text is reported */
    const char *other = "example.com/";
    d[1] = (uint8_t)(strlen(other) + 5);
    d[4] = (uint8_t)(strlen(other) + 1);
    memcpy(d + 7, other, strlen(other));
    d[7 + strlen(other)] = 0xFE;
    uint8_t buf[1024];
    char link[NFC_TAG_LINK_MAX];
    t2t_io_t io = io_for(&t);
    CHECK(t2t_read_ref(&io, buf, sizeof buf, &got, link, sizeof link, &o) == T2T_ERR_TAG);
    CHECK(o.tag_err == NFC_TAG_ERR_NOT_SPOTIFY && strcmp(link, "https://example.com/") == 0);
}

int main(void)
{
    test_write_read(213);
    test_write_read(215);
    test_write_read(216);
    test_unformatted(213, (const uint8_t *)"\xE1\x10\x12\x00");
    test_unformatted(215, (const uint8_t *)"\xE1\x10\x3E\x00");
    test_unformatted(216, (const uint8_t *)"\xE1\x10\x6D\x00");
    test_protection();
    test_verify();
    test_tag_loss();
    test_long_read();
    test_foreign_content();
    printf("t2t: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
