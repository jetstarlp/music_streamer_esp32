/* Hex, HTML, form and URL helpers. */
#include "net_pure.h"

#include <string.h>

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool net_hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    if (hex == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hi < 0 ? -1 : hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return hex[2 * out_len] == '\0';
}

bool net_html_escape(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (in == NULL) {
        return true;
    }
    size_t o = 0;
    for (const char *p = in; *p != '\0'; p++) {
        const char *rep = NULL;
        char one[2] = {*p, '\0'};
        switch (*p) {
        case '&':  rep = "&amp;"; break;
        case '<':  rep = "&lt;"; break;
        case '>':  rep = "&gt;"; break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default:
            if ((unsigned char)*p < 0x20 || *p == 0x7F) {
                one[0] = '?';
            }
            rep = one;
            break;
        }
        size_t n = strlen(rep);
        if (o + n >= cap) {
            out[o] = '\0';
            return false;
        }
        memcpy(out + o, rep, n);
        o += n;
    }
    out[o] = '\0';
    return true;
}

/* URL-decode src[0..len) into out; false on overflow, NUL or bad escapes. */
static bool url_decode(const char *src, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= len) {
                return false;   /* truncated escape */
            }
            int hi = hex_val(src[i + 1]);
            int lo = hi < 0 ? -1 : hex_val(src[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0' || o + 1 >= cap) {
            return false;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

bool net_form_get(const char *body, size_t len, const char *key, char *out, size_t cap)
{
    if (body == NULL || key == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    const size_t klen = strlen(key);
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && body[end] != '&') {
            end++;
        }
        /* field = body[pos..end) */
        if (end - pos >= klen + 1 && memcmp(body + pos, key, klen) == 0 && body[pos + klen] == '=') {
            return url_decode(body + pos + klen + 1, end - pos - klen - 1, out, cap);
        }
        if (end - pos == klen && memcmp(body + pos, key, klen) == 0) {
            return true;    /* "key" without "=": empty value */
        }
        pos = end + 1;
    }
    return false;
}

const char *net_validate_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return "Bitte den Namen des WLANs (SSID) eingeben oder aus der Liste wählen.";
    }
    if (strlen(ssid) > NET_SSID_MAX) {
        return "Der WLAN-Name ist zu lang (höchstens 32 Zeichen).";
    }
    for (const char *p = ssid; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) {
            return "Der WLAN-Name enthält ungültige Zeichen.";
        }
    }
    if (password == NULL || password[0] == '\0') {
        return NULL;    /* open network */
    }
    size_t n = strlen(password);
    if (n == 64) {
        for (const char *p = password; *p; p++) {
            if (hex_val(*p) < 0) {
                return "Ein 64 Zeichen langes Passwort darf nur aus 0-9 und a-f bestehen.";
            }
        }
        return NULL;
    }
    if (n < 8 || n > 63) {
        return "Das WLAN-Passwort muss 8 bis 63 Zeichen lang sein (bei offenen WLANs leer lassen).";
    }
    for (const char *p = password; *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) {
            return "Das WLAN-Passwort enthält Zeichen, die WLAN nicht erlaubt (z. B. Umlaute).";
        }
    }
    return NULL;
}

/* Length of "scheme://" if url starts with https:// or http://, else 0. */
static size_t scheme_len(const char *url)
{
    if (strncmp(url, "https://", 8) == 0) {
        return 8;
    }
    if (strncmp(url, "http://", 7) == 0) {
        return 7;
    }
    return 0;
}

bool net_url_valid(const char *url, bool allow_http, size_t max)
{
    if (url == NULL) {
        return false;
    }
    size_t n = strlen(url);
    if (n == 0 || n >= max) {
        return false;
    }
    size_t s = scheme_len(url);
    if (s == 0 || (s == 7 && !allow_http)) {
        return false;
    }
    if (url[s] == '\0' || url[s] == '/' || url[s] == ':' || url[s] == '?' || url[s] == '#') {
        return false;   /* no host */
    }
    for (const char *p = url; *p; p++) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

static bool join(char *out, size_t cap, const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen + blen + 1 > cap) {
        return false;
    }
    memmove(out, a, alen);
    memcpy(out + alen, b, blen + 1);
    return true;
}

bool net_url_resolve(const char *base, const char *ref, char *out, size_t cap)
{
    if (base == NULL || ref == NULL || out == NULL || cap == 0 || ref[0] == '\0') {
        return false;
    }
    if (scheme_len(ref) > 0) {
        return join(out, cap, "", 0, ref);
    }
    size_t s = scheme_len(base);
    if (s == 0) {
        return false;
    }
    if (ref[0] == '/' && ref[1] == '/') {
        return join(out, cap, base, s - 2, ref);          /* "https:" + "//host/x" */
    }
    /* end of the authority (host[:port]) */
    size_t host_end = s + strcspn(base + s, "/?#");
    if (ref[0] == '/') {
        return join(out, cap, base, host_end, ref);
    }
    /* path without query/fragment, up to and including its last '/' */
    size_t path_end = host_end + strcspn(base + host_end, "?#");
    size_t dir_end = path_end;
    while (dir_end > host_end && base[dir_end - 1] != '/') {
        dir_end--;
    }
    if (dir_end == host_end) {
        /* "https://host" or "https://host?x": directory is "/" */
        if (host_end + 1 + strlen(ref) + 1 > cap) {
            return false;
        }
        memmove(out, base, host_end);
        out[host_end] = '/';
        memcpy(out + host_end + 1, ref, strlen(ref) + 1);
        return true;
    }
    return join(out, cap, base, dir_end, ref);
}
