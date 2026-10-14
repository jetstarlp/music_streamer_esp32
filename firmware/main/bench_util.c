#include "bench_util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool bench_parse_float(const char *s, float *out)
{
    if (s == NULL || *s == '\0' || isspace((unsigned char)*s)) {
        return false;
    }
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

const char *bench_parse_tone(int argc, char **argv, bench_tone_args_t *out)
{
    bench_tone_args_t a = {
        .freq_hz = BENCH_TONE_DEFAULT_FREQ_HZ,
        .dbfs = BENCH_TONE_DEFAULT_DBFS,
        .seconds = BENCH_TONE_DEFAULT_SECONDS,
    };
    if (argc > 3) {
        return "too many arguments (usage: tone [freq_hz] [dbfs] [seconds])";
    }
    if (argc >= 1) {
        if (!bench_parse_float(argv[0], &a.freq_hz)) {
            return "frequency is not a number";
        }
        if (a.freq_hz < BENCH_TONE_MIN_FREQ_HZ || a.freq_hz > BENCH_TONE_MAX_FREQ_HZ) {
            return "frequency must be 10..20000 Hz";
        }
    }
    if (argc >= 2) {
        if (argv[1][0] == '+') {
            return "level must be 0 dBFS or below (e.g. -20)";
        }
        if (!bench_parse_float(argv[1], &a.dbfs)) {
            return "level is not a number";
        }
        if (a.dbfs > 0.0f) {
            a.dbfs = -a.dbfs;       /* "tone 1000 20" means -20 dBFS */
        }
        if (a.dbfs < BENCH_TONE_MIN_DBFS) {
            return "level must be -100..0 dBFS";
        }
    }
    if (argc >= 3) {
        if (!bench_parse_float(argv[2], &a.seconds)) {
            return "duration is not a number";
        }
        if (a.seconds < 0.1f || a.seconds > BENCH_TONE_MAX_SECONDS) {
            return "duration must be 0.1..3600 s";
        }
    }
    *out = a;
    return NULL;
}

static bool all_printable(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

const char *bench_check_ssid(const char *ssid)
{
    size_t n = ssid ? strlen(ssid) : 0;
    if (n == 0) {
        return "SSID is empty";
    }
    if (n > 32) {
        return "SSID is longer than 32 bytes";
    }
    if (!all_printable(ssid)) {
        return "SSID contains control characters";
    }
    return NULL;
}

static bool all_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

const char *bench_check_wifi_password(const char *password)
{
    size_t n = password ? strlen(password) : 0;
    if (n == 0) {
        return NULL;    /* open network */
    }
    if (!all_printable(password)) {
        return "password contains control characters";
    }
    if (n < 8) {
        return "WPA2 passwords have at least 8 characters";
    }
    if (n == 64 && !all_hex(password, n)) {
        return "a 64-character password must be a hex key";
    }
    if (n > 64) {
        return "password is longer than 63 characters";
    }
    return NULL;
}

const char *bench_check_device_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0) {
        return "name is empty";
    }
    if (n > 63) {
        return "name is longer than 63 bytes";
    }
    if (!all_printable(name)) {
        return "name contains control characters";
    }
    if (name[0] == ' ' || name[n - 1] == ' ') {
        return "name starts or ends with a space";
    }
    return NULL;
}

const char *bench_check_credential(const char *value)
{
    size_t n = value ? strlen(value) : 0;
    if (n == 0) {
        return "value is empty";
    }
    if (n > 64) {
        return "value is longer than 64 characters";
    }
    for (const char *p = value; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c >= 0x7f) {
            return "value contains spaces or non-ASCII characters";
        }
    }
    return NULL;
}

bool bench_is_hex32(const char *s)
{
    return s != NULL && strlen(s) == 32 && all_hex(s, 32);
}

void bench_mask_secret(const char *secret, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t n = secret ? strlen(secret) : 0;
    if (n == 0) {
        snprintf(out, cap, "(not set)");
        return;
    }
    /* Never reveal more than a quarter of a short value. */
    size_t show = n >= 16 ? 4 : n / 4;
    snprintf(out, cap, "****%s", secret + (n - show));
}

bool bench_join_args(int argc, char **argv, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        size_t need = n + (i > 0 ? 1 : 0);
        if (len + need >= cap) {
            out[0] = '\0';
            return false;
        }
        if (i > 0) {
            out[len++] = ' ';
        }
        memcpy(out + len, argv[i], n);
        len += n;
        out[len] = '\0';
    }
    return true;
}

bool bench_led_is_on(bench_led_mode_t mode, uint32_t ms)
{
    switch (mode) {
    case BENCH_LED_CONNECTED:
        return true;
    case BENCH_LED_PLAYING:
        return (ms % 200u) < 100u;
    case BENCH_LED_NO_WIFI:
    default:
        return (ms % 1000u) < 500u;
    }
}

int bench_merge_wifi(const settings_wifi_t *stored, int n_stored, const char *fallback_ssid,
                     const char *fallback_password, settings_wifi_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < n_stored && n < max; i++) {
        out[n++] = stored[i];
    }
    if (fallback_ssid == NULL || fallback_ssid[0] == '\0' || n >= max ||
        strlen(fallback_ssid) >= sizeof(out[0].ssid)) {
        return n;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].ssid, fallback_ssid) == 0) {
            return n;
        }
    }
    settings_wifi_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.ssid, fallback_ssid, sizeof(e.ssid) - 1);
    if (fallback_password) {
        strncpy(e.password, fallback_password, sizeof(e.password) - 1);
    }
    out[n++] = e;
    return n;
}
