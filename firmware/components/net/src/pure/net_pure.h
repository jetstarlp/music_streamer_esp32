/*
 * Pure helper functions of the net component.
 *
 * Plain C11 without any ESP-IDF dependency (net_manifest.c additionally uses
 * cJSON, which ships with ESP-IDF), so that firmware/tools/host_test_net.sh can
 * compile and test them on a PC with -fsanitize=address,undefined. Nothing in
 * here blocks or keeps state; nothing allocates except cJSON in
 * net_manifest_parse().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Scan results (net_scan.c)                                                 */
/* ------------------------------------------------------------------------- */

#define NET_SSID_MAX 32

typedef struct {
    char    ssid[NET_SSID_MAX + 1]; /* NUL-terminated; "" for hidden networks */
    int8_t  rssi;                   /* dBm */
    uint8_t channel;
    bool    secure;                 /* false: open network */
} net_ap_info_t;

/**
 * Drop entries without SSID, merge duplicates (same SSID from several access
 * points or bands; the strongest one is kept) and sort by RSSI, strongest
 * first. Entries with equal RSSI keep their relative order.
 *
 * @return new number of entries (<= n)
 */
size_t net_scan_normalize(net_ap_info_t *aps, size_t n);

/** Signal strength as 1..4 bars (for the portal list). */
int net_rssi_bars(int rssi);

/** Index of the entry with this SSID, -1 if none. */
int net_scan_find(const net_ap_info_t *aps, size_t n, const char *ssid);

/**
 * Channel for the setup hotspot: the channel of the strongest access point of
 * a stored network (`known`, n_known SSIDs), else of the strongest access
 * point at all (most likely the home network, which the user is about to
 * enter), else 1. Channels outside 1..13 are ignored. Choosing the home
 * network's channel means the hotspot does not have to change its channel when
 * the device connects to it, so the phone stays on the hotspot and sees the
 * result page.
 */
uint8_t net_pick_ap_channel(const net_ap_info_t *aps, size_t n, const char *const *known, size_t n_known);

/**
 * Name of the setup hotspot: `prefix`, with mac_suffix "-XXXX" from the last
 * two bytes of `mac` (upper-case hex). The prefix is cut to fit 32 bytes.
 * @return length written, 0 if `out` is too small or prefix is empty
 */
size_t net_portal_ssid(const char *prefix, const uint8_t mac[6], bool mac_suffix, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* WIFI QR code payload (net_wifiqr.c)                                       */
/* ------------------------------------------------------------------------- */

/**
 * Escape `in` for a field of the "WIFI:" QR format: \ ; , : " are prefixed
 * with a backslash.
 *
 * @return length written (without NUL), or 0 if `out` is too small (then
 *         out[0] is NUL). An empty input also returns 0.
 */
size_t net_wifi_qr_escape(const char *in, char *out, size_t cap);

/**
 * Build "WIFI:T:WPA;S:<ssid>;P:<password>;;" (or "WIFI:T:nopass;S:<ssid>;;"
 * when password is NULL or empty), the format Android's and iOS' cameras
 * understand.
 *
 * @return length written (without NUL), or 0 on error (empty SSID, out too small).
 */
size_t net_wifi_qr_payload(const char *ssid, const char *password, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* Versions (net_semver.c)                                                   */
/* ------------------------------------------------------------------------- */

/**
 * Compare two versions according to semver 2.0.0 precedence:
 * MAJOR.MINOR.PATCH, optional "-prerelease" (lower than the release), optional
 * "+build" (ignored). A leading 'v'/'V' is accepted, missing MINOR/PATCH count
 * as 0 ("1.2" == "1.2.0").
 *
 * @param cmp  set to -1 (a < b), 0 or 1 (a > b)
 * @return false if either string is not a valid version (cmp untouched)
 */
bool net_semver_compare(const char *a, const char *b, int *cmp);

/** True if `s` is a valid version for net_semver_compare(). */
bool net_semver_valid(const char *s);

/* ------------------------------------------------------------------------- */
/* Captive-portal DNS (net_dns.c)                                            */
/* ------------------------------------------------------------------------- */

#define NET_DNS_TTL_S 30

/**
 * Build the reply to one DNS query packet: an A/IN (or A/ANY) question is
 * answered with `ipv4` (4 bytes, network order), any other question type gets
 * an empty NOERROR answer so that clients fall back to IPv4. EDNS and other
 * records of the query are dropped.
 *
 * @return length of the reply in `out`, or 0 if nothing should be sent
 *         (malformed packet, a response, not a standard query, not exactly
 *         one question, name compression in the question, `out` too small).
 */
size_t net_dns_build_reply(const uint8_t *query, size_t qlen, const uint8_t ipv4[4],
                           uint8_t *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* Text helpers (net_text.c)                                                 */
/* ------------------------------------------------------------------------- */

/** Decode exactly 2*out_len hex characters (any case) followed by NUL. */
bool net_hex_decode(const char *hex, uint8_t *out, size_t out_len);

/**
 * HTML-escape & < > " ' (control characters become '?'). The output is always
 * NUL-terminated and never ends inside an entity.
 *
 * @return false if the output had to be truncated
 */
bool net_html_escape(const char *in, char *out, size_t cap);

/**
 * Find `key` in an application/x-www-form-urlencoded body of `len` bytes (not
 * necessarily NUL-terminated) and URL-decode its value ('+' = space, %XX).
 *
 * @return false if the key is missing, the value does not fit into `out`
 *         (cap - 1 bytes), or decodes to a NUL byte or a malformed % escape.
 */
bool net_form_get(const char *body, size_t len, const char *key, char *out, size_t cap);

/**
 * Validate what a user typed into the portal form (same rules as the console:
 * SSID 1..32 bytes; password empty (open network), 8..63 printable ASCII
 * characters or 64 hex digits).
 *
 * @return NULL if fine, otherwise a German error message for the page
 */
const char *net_validate_credentials(const char *ssid, const char *password);

/**
 * True if `url` is an absolute https:// URL (or http:// when allow_http) with
 * a host, without spaces or control characters, shorter than `max` bytes.
 */
bool net_url_valid(const char *url, bool allow_http, size_t max);

/**
 * Resolve `ref` (from a manifest) against `base` (the manifest URL), like a
 * browser does: absolute URLs are copied, "//host/x" takes the scheme of
 * base, "/x" the scheme and host of base, anything else replaces the last
 * path segment of base (query and fragment of base are dropped).
 *
 * @return false if `out` is too small or an argument is empty/invalid
 */
bool net_url_resolve(const char *base, const char *ref, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* OTA manifest (net_manifest.c, uses cJSON)                                 */
/* ------------------------------------------------------------------------- */

#define NET_URL_MAX       256
#define NET_VERSION_MAX   32
#define NET_NOTES_MAX     160
#define NET_MANIFEST_MAX  4096    /* bytes of JSON accepted */

typedef struct {
    char     version[NET_VERSION_MAX];
    char     url[NET_URL_MAX];          /* absolute, resolved against the manifest URL */
    uint8_t  sha256[32];
    uint32_t size;                      /* bytes, 0 = not given */
    char     min_version[NET_VERSION_MAX]; /* "" = not given */
    char     notes[NET_NOTES_MAX];      /* "" = not given; cut at a UTF-8 boundary */
} net_manifest_t;

/**
 * Parse and validate
 *   {"version":"1.2.0", "url":"ms-waveshare-app-1.2.0.bin", "sha256":"<64 hex>",
 *    "size":1480000, "min_version":"1.0.0", "notes":"..."}
 * version, url and sha256 are required, the rest is optional, unknown keys
 * are ignored. `url` may be relative to `manifest_url`; the resolved URL must
 * be https (or http when allow_http).
 *
 * @return NULL on success, otherwise a short English reason for the log
 */
const char *net_manifest_parse(const char *json, size_t len, const char *manifest_url,
                               bool allow_http, net_manifest_t *out);

typedef enum {
    NET_OTA_DECIDE_INSTALL = 0,     /* offered version is newer: install       */
    NET_OTA_DECIDE_UP_TO_DATE,      /* offered <= running                       */
    NET_OTA_DECIDE_TOO_OLD,         /* running < min_version: flash over USB    */
    NET_OTA_DECIDE_INVALID,         /* offered version is not a valid version   */
} net_ota_decision_t;

/**
 * The update rule. Install only when `offered` is strictly newer (semver
 * precedence) than `running` and `running` >= `min_version` (if given). A
 * running version that is no valid semver (e.g. a developer build named after
 * `git describe` before firmware/version.txt existed) counts as 0.0.0, and
 * min_version is not checked for it.
 */
net_ota_decision_t net_ota_decide(const char *running, const char *offered, const char *min_version);

/* ------------------------------------------------------------------------- */
/* Portal pages (net_pages.c)                                                */
/* ------------------------------------------------------------------------- */

/* Append-only text buffer; on overflow the content is cut (still
 * NUL-terminated) and `overflow` is set. */
typedef struct {
    char  *p;
    size_t cap;
    size_t len;
    bool   overflow;
} net_buf_t;

void net_buf_init(net_buf_t *b, char *mem, size_t cap);
void net_buf_puts(net_buf_t *b, const char *s);
/** Appends `s` HTML-escaped (see net_html_escape). */
void net_buf_put_esc(net_buf_t *b, const char *s);
void net_buf_printf(net_buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef enum {
    NET_PAGE_FORM = 0,      /* network list + form; `error` optional        */
    NET_PAGE_CONNECTING,    /* waiting for the result, reloads itself       */
    NET_PAGE_CONNECTED,     /* success with `ip`                            */
} net_page_kind_t;

typedef struct {
    net_page_kind_t      kind;
    const net_ap_info_t *aps;       /* normalized scan result (may be NULL) */
    size_t               n_aps;
    bool                 scanning;  /* no scan result yet: page reloads      */
    const char          *error;     /* German message or NULL                */
    const char          *ssid;      /* network of the attempt / prefill      */
    const char          *ip;        /* dotted quad, CONNECTED only           */
    const char          *ap_ssid;   /* name of the setup hotspot             */
    const char          *device;    /* Spotify Connect name                  */
    bool                 restarting; /* CONNECTED: the device restarts next  */
} net_page_t;

/** Render a complete HTML page (German, inline CSS, no external resources). */
void net_page_render(net_buf_t *b, const net_page_t *page);

#ifdef __cplusplus
}
#endif
