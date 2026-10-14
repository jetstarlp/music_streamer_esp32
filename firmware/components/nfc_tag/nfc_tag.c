/*
 * nfc_tag - Spotify links on NFC Forum Type 2 tags. See include/nfc_tag.h.
 *
 * Pure C99: no ESP-IDF, no heap. Every parser works on caller-provided
 * memory with explicit bounds; the host tests feed random bytes into all
 * entry points under ASan/UBSan (firmware/tools/host_test_nfc_tag.sh).
 */
#include "nfc_tag.h"

#include <string.h>

/* ---- Small helpers ------------------------------------------------------- */

static nfc_tag_err_t s_last_error = NFC_TAG_OK;   /* only state in this module */

static bool is_base62(unsigned c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static unsigned lower(unsigned c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool is_space(unsigned c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static void put_empty(char *out, size_t cap)
{
    if (out && cap > 0) {
        out[0] = '\0';
    }
}

/* ---- Character sources -----------------------------------------------------
 * A link is read through a small view so that a URI record (prefix string +
 * payload bytes) and a UTF-16 Text record can be parsed without copying them
 * into a buffer of limited size. */

enum { ENC_8BIT, ENC_UTF16BE, ENC_UTF16LE };

typedef struct {
    const char    *pre;      /* expanded URI identifier code, "" if none        */
    size_t         pre_len;
    const uint8_t *body;     /* 8-bit characters or UTF-16 code units           */
    size_t         body_len; /* in characters                                   */
    int            enc;
} src_t;

typedef struct {
    const src_t *src;
    size_t       off;        /* first character of the view inside src          */
    size_t       len;
} view_t;

/* Character i of the logical string pre + body; UTF-16 units above 0x7F are
 * returned as they are (never a link character). */
static unsigned src_at(const src_t *s, size_t i)
{
    if (i < s->pre_len) {
        return (unsigned char)s->pre[i];
    }
    i -= s->pre_len;
    if (s->enc == ENC_UTF16BE) {
        return (unsigned)s->body[2 * i] << 8 | s->body[2 * i + 1];
    }
    if (s->enc == ENC_UTF16LE) {
        return (unsigned)s->body[2 * i + 1] << 8 | s->body[2 * i];
    }
    return s->body[i];
}

static unsigned vc(const view_t *v, size_t i)
{
    return src_at(v->src, v->off + i);
}

/* [a, b) equals lit, ASCII case-insensitive. */
static bool seg_eq(const view_t *v, size_t a, size_t b, const char *lit)
{
    size_t n = strlen(lit);
    if (b - a != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower(vc(v, a + i)) != (unsigned char)lit[i]) {
            return false;
        }
    }
    return true;
}

/* [a, b) starts with lit, ASCII case-insensitive. */
static bool seg_starts(const view_t *v, size_t a, size_t b, const char *lit)
{
    size_t n = strlen(lit);
    return b - a >= n && seg_eq(v, a, a + n, lit);
}

/* [a, b) ends with lit, ASCII case-insensitive. */
static bool seg_ends(const view_t *v, size_t a, size_t b, const char *lit)
{
    size_t n = strlen(lit);
    return b - a >= n && seg_eq(v, b - n, b, lit);
}

/* Copies the view into out (truncated, always terminated). Characters that do
 * not fit into a C string (NUL, UTF-16 above 0xFF) become '?'. */
static void copy_view(const view_t *v, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    size_t n = v->len < cap - 1 ? v->len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned c = vc(v, i);
        out[i] = (c == 0 || c > 0xFF) ? '?' : (char)c;
    }
    out[n] = '\0';
}

/* ---- Spotify references --------------------------------------------------- */

static const char *const s_type_names[] = {
    "track", "album", "playlist", "artist", "show", "episode",
};
#define TYPE_COUNT (sizeof s_type_names / sizeof s_type_names[0])

const char *spotify_ref_type_name(spotify_ref_type_t type)
{
    unsigned t = (unsigned)type;
    return t < TYPE_COUNT ? s_type_names[t] : NULL;
}

bool spotify_ref_is_valid(const spotify_ref_t *ref)
{
    if (!ref || !spotify_ref_type_name(ref->type)) {
        return false;
    }
    for (size_t i = 0; i < SPOTIFY_REF_ID_LEN; i++) {
        if (!is_base62((unsigned char)ref->id[i])) {
            return false;
        }
    }
    return ref->id[SPOTIFY_REF_ID_LEN] == '\0';
}

bool spotify_ref_equal(const spotify_ref_t *a, const spotify_ref_t *b)
{
    return spotify_ref_is_valid(a) && spotify_ref_is_valid(b) && a->type == b->type &&
           memcmp(a->id, b->id, SPOTIFY_REF_ID_LEN) == 0;
}

/* Writes prefix + type + sep + id; "" if ref is invalid or cap too small. */
static void ref_format(const spotify_ref_t *ref, const char *prefix, char sep, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!spotify_ref_is_valid(ref)) {
        return;
    }
    const char *tn = spotify_ref_type_name(ref->type);
    size_t lp = strlen(prefix), lt = strlen(tn);
    if (lp + lt + 1 + SPOTIFY_REF_ID_LEN + 1 > cap) {
        return;
    }
    memcpy(out, prefix, lp);
    memcpy(out + lp, tn, lt);
    out[lp + lt] = sep;
    memcpy(out + lp + lt + 1, ref->id, SPOTIFY_REF_ID_LEN + 1);
}

void spotify_ref_to_uri(const spotify_ref_t *ref, char *out, size_t cap)
{
    ref_format(ref, "spotify:", ':', out, cap);
}

void spotify_ref_to_url(const spotify_ref_t *ref, char *out, size_t cap)
{
    ref_format(ref, "https://open.spotify.com/", '/', out, cap);
}

/* ---- Link parser ------------------------------------------------------------ */

#define MAX_SEGS 8

typedef struct {
    size_t a[MAX_SEGS], b[MAX_SEGS];
    int    n;
    bool   overflow;   /* more than MAX_SEGS segments */
} segs_t;

static bool type_from_seg(const view_t *v, size_t a, size_t b, spotify_ref_type_t *type)
{
    for (unsigned t = 0; t < TYPE_COUNT; t++) {
        if (seg_eq(v, a, b, s_type_names[t])) {
            *type = (spotify_ref_type_t)t;
            return true;
        }
    }
    return false;
}

static nfc_tag_err_t set_ref(const view_t *v, size_t a, size_t b, spotify_ref_type_t type,
                             spotify_ref_t *out)
{
    if (b - a != SPOTIFY_REF_ID_LEN) {
        return NFC_TAG_ERR_BAD_ID;
    }
    for (size_t i = 0; i < SPOTIFY_REF_ID_LEN; i++) {
        unsigned c = vc(v, a + i);
        if (!is_base62(c)) {
            return NFC_TAG_ERR_BAD_ID;
        }
        out->id[i] = (char)c;
    }
    out->id[SPOTIFY_REF_ID_LEN] = '\0';
    out->type = type;
    return NFC_TAG_OK;
}

/* Evaluates the segments behind the host or the "spotify:" scheme:
 * <type> <id> or the legacy user <name> playlist <id>. */
static nfc_tag_err_t ref_from_segs(const view_t *v, const segs_t *s, spotify_ref_t *out)
{
    spotify_ref_type_t type;
    if (s->overflow || s->n == 0) {
        return NFC_TAG_ERR_UNSUPPORTED_TYPE;
    }
    if (type_from_seg(v, s->a[0], s->b[0], &type)) {
        if (s->n == 1) {
            return NFC_TAG_ERR_BAD_ID;              /* "track" without id    */
        }
        if (s->n > 2) {
            return NFC_TAG_ERR_UNSUPPORTED_TYPE;    /* sub-page of an entity */
        }
        return set_ref(v, s->a[1], s->b[1], type, out);
    }
    if (seg_eq(v, s->a[0], s->b[0], "user") && s->n >= 3 && s->b[1] > s->a[1] &&
        seg_eq(v, s->a[2], s->b[2], "playlist")) {
        if (s->n == 3) {
            return NFC_TAG_ERR_BAD_ID;
        }
        if (s->n > 4) {
            return NFC_TAG_ERR_UNSUPPORTED_TYPE;
        }
        return set_ref(v, s->a[3], s->b[3], SPOTIFY_REF_PLAYLIST, out);
    }
    return NFC_TAG_ERR_UNSUPPORTED_TYPE;            /* user, genre, collection, ... */
}

/* Splits [p, e) at sep. skip_empty drops empty segments (URL paths). */
static void split(const view_t *v, size_t p, size_t e, unsigned sep, bool skip_empty, segs_t *s)
{
    s->n = 0;
    s->overflow = false;
    while (p <= e) {
        size_t q = p;
        while (q < e && vc(v, q) != sep) {
            q++;
        }
        if (!(skip_empty && q == p)) {
            if (s->n == MAX_SEGS) {
                s->overflow = true;
                return;
            }
            s->a[s->n] = p;
            s->b[s->n] = q;
            s->n++;
        }
        p = q + 1;
    }
}

/* First '?' or '#' in [p, e), else e. */
static size_t query_start(const view_t *v, size_t p, size_t e)
{
    while (p < e && vc(v, p) != '?' && vc(v, p) != '#') {
        p++;
    }
    return p;
}

static nfc_tag_err_t parse_link_view(const view_t *v, spotify_ref_t *out)
{
    size_t b = 0, e = v->len;
    while (b < e && is_space(vc(v, b))) {
        b++;
    }
    while (e > b && is_space(vc(v, e - 1))) {
        e--;
    }
    if (b == e) {
        return NFC_TAG_ERR_NO_LINK;
    }

    segs_t s;
    if (seg_starts(v, b, e, "spotify:")) {           /* spotify:<type>:<id> */
        split(v, b + 8, query_start(v, b + 8, e), ':', false, &s);
        return ref_from_segs(v, &s, out);
    }

    size_t p = b;                                    /* [scheme] host path */
    if (seg_starts(v, p, e, "https://")) {
        p += 8;
    } else if (seg_starts(v, p, e, "http://")) {
        p += 7;
    } else {
        /* Any other scheme (tel:, mailto:, urn:, ftp://, ...) is not Spotify. */
        while (p < e && (is_base62(vc(v, p)) || vc(v, p) == '+' || vc(v, p) == '-')) {
            p++;
        }
        if (p > b && p < e && vc(v, p) == ':') {
            return NFC_TAG_ERR_NOT_SPOTIFY;
        }
        p = b;
    }
    size_t h0 = p;
    while (p < e && vc(v, p) != '/' && vc(v, p) != '?' && vc(v, p) != '#') {
        p++;
    }
    size_t h1 = p;

    if (seg_eq(v, h0, h1, "spotify.link") || seg_ends(v, h0, h1, ".spotify.link") ||
        seg_eq(v, h0, h1, "spotify.app.link") || seg_eq(v, h0, h1, "spoti.fi")) {
        return NFC_TAG_ERR_SHORT_LINK;
    }
    if (!seg_eq(v, h0, h1, "open.spotify.com") && !seg_eq(v, h0, h1, "play.spotify.com")) {
        return (seg_eq(v, h0, h1, "spotify.com") || seg_ends(v, h0, h1, ".spotify.com"))
                   ? NFC_TAG_ERR_UNSUPPORTED_TYPE : NFC_TAG_ERR_NOT_SPOTIFY;
    }

    split(v, h1, query_start(v, h1, e), '/', true, &s);
    /* Drop leading "intl-xx" (localised links) and "embed" (web player widget). */
    int skip = 0;
    while (skip < s.n && skip < 2 &&
           ((seg_starts(v, s.a[skip], s.b[skip], "intl-") && s.b[skip] - s.a[skip] > 5) ||
            seg_eq(v, s.a[skip], s.b[skip], "embed"))) {
        skip++;
    }
    for (int i = skip; i < s.n; i++) {
        s.a[i - skip] = s.a[i];
        s.b[i - skip] = s.b[i];
    }
    s.n -= skip;
    return ref_from_segs(v, &s, out);
}

nfc_tag_err_t nfc_tag_parse_link_ex(const char *text, spotify_ref_t *out)
{
    if (!text || !out) {
        return NFC_TAG_ERR_ARG;
    }
    src_t src = { "", 0, (const uint8_t *)text, strlen(text), ENC_8BIT };
    view_t v = { &src, 0, src.body_len };
    spotify_ref_t tmp;
    nfc_tag_err_t err = parse_link_view(&v, &tmp);
    if (err == NFC_TAG_OK) {
        *out = tmp;
    }
    return err;
}

bool nfc_tag_parse_link(const char *text, spotify_ref_t *out)
{
    s_last_error = nfc_tag_parse_link_ex(text, out);
    return s_last_error == NFC_TAG_OK;
}

/* ---- NDEF message ------------------------------------------------------------ */

/* NFC Forum URI RTD, URI identifier codes 0x00..0x23. */
static const char *const s_uri_prefix[] = {
    "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:",
    "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://",
    "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:",
    "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://", "irdaobex://",
    "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
    "urn:nfc:",
};
#define URI_PREFIX_COUNT (sizeof s_uri_prefix / sizeof s_uri_prefix[0])

/* Result collector while walking the records. When several records fail,
 * the most telling reason is kept (first one on equal rank):
 *   OK > Spotify link with a problem (short link, type, id) > malformed or
 *   chunked record > other link > no link at all. */
typedef struct {
    nfc_tag_err_t err;
    int           rank;
    bool          content;   /* a record other than TNF "empty" was seen */
    spotify_ref_t ref;
    char         *link;
    size_t        link_cap;
} acc_t;

static int err_rank(nfc_tag_err_t e)
{
    switch (e) {
    case NFC_TAG_OK:                   return 4;
    case NFC_TAG_ERR_SHORT_LINK:
    case NFC_TAG_ERR_UNSUPPORTED_TYPE:
    case NFC_TAG_ERR_BAD_ID:           return 3;
    case NFC_TAG_ERR_RECORD:
    case NFC_TAG_ERR_CHUNKED:          return 2;
    case NFC_TAG_ERR_NOT_SPOTIFY:      return 1;
    default:                           return 0;
    }
}

/* Records err (with the link that caused it, may be NULL) if it ranks higher. */
static void acc_offer(acc_t *a, nfc_tag_err_t err, const view_t *link)
{
    int r = err_rank(err);
    if (r <= a->rank) {
        return;
    }
    a->err = err;
    a->rank = r;
    if (link) {
        copy_view(link, a->link, a->link_cap);
    } else {
        put_empty(a->link, a->link_cap);
    }
}

/* Parses one link candidate; true when it is a valid Spotify reference. */
static bool acc_link(acc_t *a, const view_t *v)
{
    spotify_ref_t tmp;
    nfc_tag_err_t err = parse_link_view(v, &tmp);
    if (err == NFC_TAG_OK && a->rank < err_rank(NFC_TAG_OK)) {
        a->ref = tmp;
    }
    acc_offer(a, err, v);
    return err == NFC_TAG_OK;
}

/* Characters that end a link inside free text. */
static bool is_text_sep(unsigned c)
{
    return c <= ' ' || c >= 0x7F || c == '"' || c == '\'' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '|' || c == '\\' || c == '^' || c == '`';
}

static bool is_trailing_punct(unsigned c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?';
}

static bool is_scheme_char(unsigned c)
{
    return is_base62(c) || c == '+' || c == '-' || c == '.';
}

/* Start of a link inside the word [a, b) of a text, or b if there is none.
 * Anchors: "<scheme>://", "www.", known Spotify hosts, "spotify:<word>:". */
static size_t find_link_start(const view_t *v, size_t a, size_t b)
{
    static const char *const hosts[] = {
        "www.", "open.spotify.com/", "play.spotify.com/", "spotify.link/", "spoti.fi/",
        "spotify.app.link/",
    };
    for (size_t i = a; i < b; i++) {
        if (seg_starts(v, i, b, "://")) {
            size_t s = i;
            while (s > a && is_scheme_char(vc(v, s - 1))) {
                s--;
            }
            return s;
        }
        for (size_t h = 0; h < sizeof hosts / sizeof hosts[0]; h++) {
            if (seg_starts(v, i, b, hosts[h])) {
                return i;
            }
        }
        if (seg_starts(v, i, b, "spotify:")) {
            /* "spotify:track:..." but not the label in "Spotify: https://..." */
            size_t j = i + 8;
            while (j < b && ((vc(v, j) >= 'a' && vc(v, j) <= 'z') ||
                             (vc(v, j) >= 'A' && vc(v, j) <= 'Z'))) {
                j++;
            }
            if (j > i + 8 && j < b && vc(v, j) == ':' && (j + 1 >= b || vc(v, j + 1) != '/')) {
                return i;
            }
        }
    }
    return b;
}

/* Looks for links in a text; true when a valid Spotify link was found. */
static bool acc_text(acc_t *a, const view_t *text)
{
    size_t i = 0, n = text->len;
    while (i < n) {
        while (i < n && is_text_sep(vc(text, i))) {
            i++;
        }
        size_t w0 = i;
        while (i < n && !is_text_sep(vc(text, i))) {
            i++;
        }
        size_t w1 = i;
        /* Sentence punctuation after a link is not part of it. */
        while (w1 > w0 && is_trailing_punct(vc(text, w1 - 1))) {
            w1--;
        }
        size_t s = find_link_start(text, w0, w1);
        if (s < w1) {
            view_t link = { text->src, text->off + s, w1 - s };
            if (acc_link(a, &link)) {
                return true;
            }
        }
    }
    return false;
}

static bool scan_message(const uint8_t *m, size_t n, int depth, acc_t *a);

#define NDEF_MB  0x80
#define NDEF_ME  0x40
#define NDEF_CF  0x20
#define NDEF_SR  0x10
#define NDEF_IL  0x08

#define TNF_EMPTY      0
#define TNF_WELL_KNOWN 1
#define TNF_ABS_URI    3
#define TNF_UNCHANGED  6
#define TNF_RESERVED   7

/* Evaluates one (unchunked) record. depth 1 = inside a Smart Poster, where
 * only the URI record counts (Text records there are titles). */
static bool eval_record(int tnf, const uint8_t *type, size_t tl, const uint8_t *pay, size_t pl,
                        int depth, acc_t *a)
{
    if (tnf == TNF_WELL_KNOWN && tl == 1 && type[0] == 'U') {
        if (pl == 0 || pay[0] >= URI_PREFIX_COUNT) {
            acc_offer(a, NFC_TAG_ERR_RECORD, NULL);
            return false;
        }
        const char *pre = s_uri_prefix[pay[0]];
        src_t src = { pre, strlen(pre), pay + 1, pl - 1, ENC_8BIT };
        view_t v = { &src, 0, src.pre_len + src.body_len };
        return acc_link(a, &v);
    }
    if (depth > 0) {
        return false;
    }
    if (tnf == TNF_WELL_KNOWN && tl == 1 && type[0] == 'T') {
        /* status byte: bit 7 UTF-16, bits 5..0 length of the language code */
        if (pl == 0 || 1u + (pay[0] & 0x3Fu) > pl) {
            acc_offer(a, NFC_TAG_ERR_RECORD, NULL);
            return false;
        }
        size_t skip = 1u + (pay[0] & 0x3Fu);
        src_t src = { "", 0, pay + skip, pl - skip, ENC_8BIT };
        if (pay[0] & 0x80) {
            src.enc = ENC_UTF16BE;                    /* default without BOM */
            src.body_len /= 2;
            if (src.body_len > 0 && src.body[0] == 0xFF && src.body[1] == 0xFE) {
                src.enc = ENC_UTF16LE;
            }
            if (src.body_len > 0 && src_at(&src, 0) == 0xFEFF) {
                src.body += 2;
                src.body_len--;
            }
        }
        view_t v = { &src, 0, src.body_len };
        return acc_text(a, &v);
    }
    if (tnf == TNF_WELL_KNOWN && tl == 2 && type[0] == 'S' && type[1] == 'p') {
        return scan_message(pay, pl, depth + 1, a);
    }
    if (tnf == TNF_ABS_URI) {
        /* The URI belongs in the type field; some writers use the payload. */
        src_t src = { "", 0, tl ? type : pay, tl ? tl : pl, ENC_8BIT };
        view_t v = { &src, 0, src.body_len };
        return acc_link(a, &v);
    }
    return false;                                     /* AAR, MIME, ... */
}

/* Walks the records of one NDEF message; true when a valid link was found. */
static bool scan_message(const uint8_t *m, size_t n, int depth, acc_t *a)
{
    size_t p = 0;
    while (p < n) {
        uint8_t h = m[p];
        size_t hdr = 2u + ((h & NDEF_SR) ? 1u : 4u) + ((h & NDEF_IL) ? 1u : 0u);
        if (n - p < hdr) {
            acc_offer(a, NFC_TAG_ERR_RECORD, NULL);
            return false;
        }
        size_t q = p + 1;
        size_t tl = m[q++], pl, il = 0;
        if (h & NDEF_SR) {
            pl = m[q++];
        } else {
            uint32_t l32 = (uint32_t)m[q] << 24 | (uint32_t)m[q + 1] << 16 |
                           (uint32_t)m[q + 2] << 8 | m[q + 3];
            pl = l32;
            q += 4;
        }
        if (h & NDEF_IL) {
            il = m[q++];
        }
        size_t rem = n - q;
        if (tl > rem || il > rem - tl || pl > rem - tl - il) {
            acc_offer(a, NFC_TAG_ERR_RECORD, NULL);   /* record runs past the message */
            return false;
        }
        const uint8_t *type = m + q, *pay = m + q + tl + il;
        p = q + tl + il + pl;

        int tnf = h & 0x07;
        if ((h & NDEF_CF) || tnf == TNF_UNCHANGED) {
            acc_offer(a, NFC_TAG_ERR_CHUNKED, NULL);
        } else if (tnf == TNF_RESERVED || (tnf == TNF_EMPTY && (tl | il | pl) != 0)) {
            acc_offer(a, NFC_TAG_ERR_RECORD, NULL);
        } else if (tnf != TNF_EMPTY) {
            a->content = true;
            if (eval_record(tnf, type, tl, pay, pl, depth, a)) {
                return true;
            }
        }
        if (h & NDEF_ME) {
            break;    /* bytes after the last record are ignored */
        }
    }
    return false;
}

/* ---- TLV walk (Type 2 tag data area) --------------------------------------- */

#define TLV_NULL  0x00
#define TLV_LOCK  0x01
#define TLV_MEM   0x02
#define TLV_NDEF  0x03
#define TLV_TERM  0xFE

typedef struct {
    nfc_tag_err_t err;   /* OK: NDEF TLV with value at val/vlen                 */
    size_t        need;  /* bytes needed to decide (see nfc_tag_ndef_bytes_needed) */
    size_t        val, vlen;
} tlv_find_t;

static void tlv_find_ndef(const uint8_t *mem, size_t len, tlv_find_t *r)
{
    size_t pos = 0;
    memset(r, 0, sizeof *r);
    for (;;) {
        if (pos >= len) {                        /* end of data between TLVs */
            r->err = NFC_TAG_ERR_NO_NDEF;
            r->need = pos + 1;
            return;
        }
        uint8_t t = mem[pos];
        if (t == TLV_NULL) {
            pos++;
            continue;
        }
        if (t == TLV_TERM) {
            r->err = NFC_TAG_ERR_NO_NDEF;
            r->need = pos + 1;
            return;
        }
        size_t hdr = 2, l;
        if (len - pos < 2) {
            r->err = NFC_TAG_ERR_TRUNCATED;
            r->need = pos + 2;
            return;
        }
        l = mem[pos + 1];
        if (l == 0xFF) {                         /* 3-byte length form FF hi lo */
            hdr = 4;
            if (len - pos < 4) {
                r->err = NFC_TAG_ERR_TRUNCATED;
                r->need = pos + 4;
                return;
            }
            l = (size_t)mem[pos + 2] << 8 | mem[pos + 3];
            if (l == 0xFFFF) {
                r->err = NFC_TAG_ERR_TLV;
                r->need = pos + 4;
                return;
            }
        }
        size_t vend = pos + hdr + l;
        if (t == TLV_NDEF) {                     /* only the first one counts */
            r->need = vend;
            if (vend > len) {
                r->err = NFC_TAG_ERR_TRUNCATED;
            } else if (l == 0) {
                r->err = NFC_TAG_ERR_EMPTY;
            } else {
                r->err = NFC_TAG_OK;
                r->val = pos + hdr;
                r->vlen = l;
            }
            return;
        }
        if (vend > len) {                        /* lock/memory control, proprietary, ... */
            r->err = NFC_TAG_ERR_TRUNCATED;
            r->need = vend + 1;
            return;
        }
        pos = vend;
    }
}

size_t nfc_tag_ndef_bytes_needed(const uint8_t *mem, size_t len)
{
    if (!mem) {
        return 0;
    }
    tlv_find_t r;
    tlv_find_ndef(mem, len, &r);
    return r.need;
}

nfc_tag_err_t nfc_tag_parse_ndef_ex(const uint8_t *mem, size_t len, spotify_ref_t *out,
                                    char *link, size_t link_cap)
{
    put_empty(link, link_cap);
    if (!mem || !out) {
        return NFC_TAG_ERR_ARG;
    }
    tlv_find_t r;
    tlv_find_ndef(mem, len, &r);
    if (r.err != NFC_TAG_OK) {
        return r.err;
    }
    acc_t a;
    memset(&a, 0, sizeof a);
    a.err = NFC_TAG_ERR_NO_LINK;
    a.link = link;
    a.link_cap = link_cap;
    scan_message(mem + r.val, r.vlen, 0, &a);
    if (a.err == NFC_TAG_OK) {
        *out = a.ref;
    } else if (!a.content && a.rank == 0) {
        return NFC_TAG_ERR_EMPTY;     /* only empty records (Android: D0 00 00) */
    }
    return a.err;
}

bool nfc_tag_parse_ndef(const uint8_t *mem, size_t len, spotify_ref_t *out)
{
    s_last_error = nfc_tag_parse_ndef_ex(mem, len, out, NULL, 0);
    return s_last_error == NFC_TAG_OK;
}

/* ---- Builder ------------------------------------------------------------------ */

nfc_tag_err_t nfc_tag_build_ndef_ex(const spotify_ref_t *ref, uint8_t *out, size_t cap,
                                    size_t *out_len)
{
    static const char host[] = "open.spotify.com/";
    if (out_len) {
        *out_len = 0;
    }
    if (!out || !spotify_ref_is_valid(ref)) {
        return NFC_TAG_ERR_ARG;
    }
    const char *tn = spotify_ref_type_name(ref->type);
    size_t lh = sizeof host - 1, lt = strlen(tn);
    size_t plen = 1 + lh + lt + 1 + SPOTIFY_REF_ID_LEN;   /* prefix code + text */
    size_t rlen = 4 + plen;                                /* D1 01 plen 55 + payload */
    size_t total = 2 + rlen + 1;                           /* 03 len ... FE */
    if (total > cap) {
        return NFC_TAG_ERR_NO_SPACE;
    }
    uint8_t *p = out;
    *p++ = TLV_NDEF;
    *p++ = (uint8_t)rlen;
    *p++ = NDEF_MB | NDEF_ME | NDEF_SR | TNF_WELL_KNOWN;  /* 0xD1 */
    *p++ = 1;                                              /* type length */
    *p++ = (uint8_t)plen;
    *p++ = 'U';
    *p++ = 0x04;                                           /* "https://" */
    memcpy(p, host, lh);
    p += lh;
    memcpy(p, tn, lt);
    p += lt;
    *p++ = '/';
    memcpy(p, ref->id, SPOTIFY_REF_ID_LEN);
    p += SPOTIFY_REF_ID_LEN;
    *p++ = TLV_TERM;
    if (out_len) {
        *out_len = (size_t)(p - out);
    }
    return NFC_TAG_OK;
}

size_t nfc_tag_build_ndef(const spotify_ref_t *ref, uint8_t *out, size_t cap)
{
    size_t n = 0;
    s_last_error = nfc_tag_build_ndef_ex(ref, out, cap, &n);
    return n;
}

/* ---- Type 2 tag helpers ----------------------------------------------------------- */

nfc_tag_err_t nfc_tag_t2t_check_cc(const uint8_t cc[4], bool for_write, size_t *data_size)
{
    if (!cc) {
        return NFC_TAG_ERR_ARG;
    }
    if (cc[0] != 0xE1 || cc[2] == 0) {
        return NFC_TAG_ERR_NOT_FORMATTED;
    }
    if ((cc[1] >> 4) != 1 || (cc[3] >> 4) != 0) {
        return NFC_TAG_ERR_UNSUPPORTED;     /* mapping version 2+, proprietary read access */
    }
    if (for_write && (cc[3] & 0x0F) != 0) {
        return NFC_TAG_ERR_READ_ONLY;       /* 0xF = no write access, others proprietary */
    }
    if (data_size) {
        *data_size = (size_t)cc[2] * 8;
    }
    return NFC_TAG_OK;
}

nfc_tag_err_t nfc_tag_t2t_check_static_lock(const uint8_t page2[4])
{
    if (!page2) {
        return NFC_TAG_ERR_ARG;
    }
    return (page2[2] | page2[3]) ? NFC_TAG_ERR_READ_ONLY : NFC_TAG_OK;
}

nfc_tag_err_t nfc_tag_ntag_from_version(const uint8_t ver[8], nfc_tag_ntag_info_t *info)
{
    if (!ver || !info) {
        return NFC_TAG_ERR_ARG;
    }
    /* 00 04 04 02 01 00 <size> 03; subtype 02 = NTAG21x, 04 = NTAG21xF (same
     * memory map). Subtype 05 (NTAG I2C) shares the size byte 0x13 but not
     * the layout and is rejected. */
    if (ver[1] != 0x04 || ver[2] != 0x04 || (ver[3] != 0x02 && ver[3] != 0x04)) {
        return NFC_TAG_ERR_UNSUPPORTED;
    }
    static const nfc_tag_ntag_info_t models[] = {
        { 213, 45, 144, 0x28 },
        { 215, 135, 504, 0x82 },
        { 216, 231, 888, 0xE2 },
    };
    static const uint8_t sizes[] = { 0x0F, 0x11, 0x13 };
    for (size_t i = 0; i < sizeof sizes; i++) {
        if (ver[6] == sizes[i]) {
            *info = models[i];
            return NFC_TAG_OK;
        }
    }
    return NFC_TAG_ERR_UNSUPPORTED;
}

nfc_tag_err_t nfc_tag_ntag_check_config(const nfc_tag_ntag_info_t *info, const uint8_t cfg[16],
                                        uint16_t last_page)
{
    if (!info || !cfg || last_page < NFC_TAG_T2T_FIRST_DATA_PAGE ||
        last_page >= info->dyn_lock_page) {
        return NFC_TAG_ERR_ARG;
    }
    if (cfg[0] | cfg[1] | cfg[2]) {       /* dynamic lock bytes 0..2 */
        return NFC_TAG_ERR_READ_ONLY;
    }
    if (cfg[7] <= last_page) {            /* CFG0 byte 3 = AUTH0 */
        return NFC_TAG_ERR_PASSWORD;
    }
    return NFC_TAG_OK;
}

/* Walks the lock/memory control TLVs (and NULL TLVs between them) at the start
 * of the data area. *keep = end of the last control TLV. If wr_hi > wr_lo, an
 * area reserved by a control TLV that overlaps data-area bytes [wr_lo, wr_hi)
 * gives NFC_TAG_ERR_UNSUPPORTED. */
static nfc_tag_err_t t2t_control_tlvs(const uint8_t *cur, size_t cur_len, size_t wr_lo,
                                      size_t wr_hi, size_t *keep)
{
    size_t pos = 0;
    *keep = 0;
    while (pos < cur_len) {
        uint8_t t = cur[pos];
        if (t == TLV_NULL) {
            pos++;
            continue;
        }
        if (t != TLV_LOCK && t != TLV_MEM) {
            break;              /* NDEF, terminator or other: the new NDEF TLV goes here */
        }
        if (cur_len - pos < 2) {
            return NFC_TAG_ERR_TRUNCATED;
        }
        if (cur[pos + 1] != 3) {
            return NFC_TAG_ERR_UNSUPPORTED;
        }
        if (cur_len - pos < 5) {
            return NFC_TAG_ERR_TRUNCATED;
        }
        /* Position: page (high nibble) * 2^MOS + byte offset (low nibble), as
         * an absolute byte address from page 0. Size: bits (lock) or bytes
         * (memory), 0 meaning 256. */
        const uint8_t *v = cur + pos + 2;
        size_t addr = (size_t)(v[0] >> 4) * ((size_t)1 << (v[2] & 0x0F)) + (v[0] & 0x0F);
        size_t size = v[1] ? v[1] : 256;
        if (t == TLV_LOCK) {
            size = (size + 7) / 8;
        }
        size_t lo = NFC_TAG_T2T_FIRST_DATA_PAGE * NFC_TAG_T2T_PAGE_SIZE + wr_lo;
        size_t hi = NFC_TAG_T2T_FIRST_DATA_PAGE * NFC_TAG_T2T_PAGE_SIZE + wr_hi;
        if (wr_hi > wr_lo && addr < hi && addr + size > lo) {
            return NFC_TAG_ERR_UNSUPPORTED;
        }
        pos += 5;
        *keep = pos;
    }
    return NFC_TAG_OK;
}

nfc_tag_err_t nfc_tag_t2t_plan_write(const uint8_t *cur, size_t cur_len, size_t data_size,
                                     const spotify_ref_t *ref, nfc_tag_t2t_plan_t *plan)
{
    if (!plan || (!cur && cur_len) || !spotify_ref_is_valid(ref)) {
        return NFC_TAG_ERR_ARG;
    }
    memset(plan, 0, sizeof *plan);

    size_t keep;
    nfc_tag_err_t err = t2t_control_tlvs(cur, cur_len, 0, 0, &keep);
    if (err != NFC_TAG_OK) {
        return err;
    }
    if (keep >= NFC_TAG_T2T_PLAN_MAX) {
        return NFC_TAG_ERR_NO_SPACE;
    }
    if (keep) {
        memcpy(plan->img, cur, keep);
    }
    size_t n;
    err = nfc_tag_build_ndef_ex(ref, plan->img + keep, sizeof plan->img - keep, &n);
    if (err != NFC_TAG_OK) {
        memset(plan, 0, sizeof *plan);
        return err;
    }
    size_t end = (keep + n + 3) & ~(size_t)3;
    if (keep + n > data_size || end > sizeof plan->img) {
        memset(plan, 0, sizeof *plan);
        return NFC_TAG_ERR_NO_SPACE;
    }
    /* img beyond keep + n is still 0x00 from the memset: page padding. */
    plan->end = end;
    plan->ndef_offset = keep;
    plan->len_offset = keep + 1;
    plan->first_page = (uint16_t)(NFC_TAG_T2T_FIRST_DATA_PAGE + keep / NFC_TAG_T2T_PAGE_SIZE);
    plan->len_page = (uint16_t)(NFC_TAG_T2T_FIRST_DATA_PAGE + plan->len_offset / NFC_TAG_T2T_PAGE_SIZE);
    plan->last_page = (uint16_t)(NFC_TAG_T2T_FIRST_DATA_PAGE + end / NFC_TAG_T2T_PAGE_SIZE - 1);

    size_t wr_lo = (size_t)(plan->first_page - NFC_TAG_T2T_FIRST_DATA_PAGE) * NFC_TAG_T2T_PAGE_SIZE;
    err = t2t_control_tlvs(cur, cur_len, wr_lo, end, &keep);
    if (err != NFC_TAG_OK) {
        memset(plan, 0, sizeof *plan);
        return err;
    }
    return NFC_TAG_OK;
}

/* ---- Errors --------------------------------------------------------------------------- */

nfc_tag_err_t nfc_tag_last_error(void)
{
    return s_last_error;
}

const char *nfc_tag_err_str(nfc_tag_err_t err)
{
    switch (err) {
    case NFC_TAG_OK:                   return "ok";
    case NFC_TAG_ERR_ARG:              return "invalid argument";
    case NFC_TAG_ERR_NO_NDEF:          return "no NDEF message on the tag";
    case NFC_TAG_ERR_EMPTY:            return "empty NDEF message";
    case NFC_TAG_ERR_TLV:              return "malformed TLV";
    case NFC_TAG_ERR_TRUNCATED:        return "tag data truncated, read more";
    case NFC_TAG_ERR_RECORD:           return "malformed NDEF record";
    case NFC_TAG_ERR_CHUNKED:          return "chunked NDEF records not supported";
    case NFC_TAG_ERR_NO_LINK:          return "no link on the tag";
    case NFC_TAG_ERR_NOT_SPOTIFY:      return "link is not a Spotify link";
    case NFC_TAG_ERR_SHORT_LINK:       return "Spotify short link needs a network lookup";
    case NFC_TAG_ERR_UNSUPPORTED_TYPE: return "unsupported kind of Spotify link";
    case NFC_TAG_ERR_BAD_ID:           return "invalid Spotify id";
    case NFC_TAG_ERR_NO_SPACE:         return "not enough space";
    case NFC_TAG_ERR_NOT_FORMATTED:    return "tag not NDEF formatted";
    case NFC_TAG_ERR_READ_ONLY:        return "tag is read-only";
    case NFC_TAG_ERR_PASSWORD:         return "tag is password protected";
    case NFC_TAG_ERR_UNSUPPORTED:      return "tag type or layout not supported";
    }
    return "unknown error";
}
