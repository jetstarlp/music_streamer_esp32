/* "WIFI:" QR code payload (ZXing format, understood by Android and iOS cameras). */
#include "net_pure.h"

#include <string.h>

static bool needs_escape(char c)
{
    return c == '\\' || c == ';' || c == ',' || c == ':' || c == '"';
}

size_t net_wifi_qr_escape(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }
    size_t o = 0;
    for (const char *p = in; *p != '\0'; p++) {
        size_t need = needs_escape(*p) ? 2 : 1;
        if (o + need >= cap) { /* keep room for the NUL */
            out[0] = '\0';
            return 0;
        }
        if (need == 2) {
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return o;
}

/* Append a literal; false if it does not fit (incl. NUL). */
static bool append(char *out, size_t cap, size_t *o, const char *s)
{
    size_t len = strlen(s);
    if (*o + len >= cap) {
        return false;
    }
    memcpy(out + *o, s, len + 1);
    *o += len;
    return true;
}

static bool append_escaped(char *out, size_t cap, size_t *o, const char *s)
{
    if (*o >= cap) {
        return false;
    }
    size_t n = net_wifi_qr_escape(s, out + *o, cap - *o);
    if (n == 0 && s[0] != '\0') {
        return false;
    }
    *o += n;
    return true;
}

size_t net_wifi_qr_payload(const char *ssid, const char *password, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (ssid == NULL || ssid[0] == '\0') {
        return 0;
    }
    const bool open = (password == NULL || password[0] == '\0');
    size_t o = 0;
    bool ok = append(out, cap, &o, open ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:") &&
              append_escaped(out, cap, &o, ssid) &&
              append(out, cap, &o, ";");
    if (ok && !open) {
        ok = append(out, cap, &o, "P:") && append_escaped(out, cap, &o, password) &&
             append(out, cap, &o, ";");
    }
    ok = ok && append(out, cap, &o, ";");
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return o;
}
