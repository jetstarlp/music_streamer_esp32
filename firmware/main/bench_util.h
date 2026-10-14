/*
 * bench_util - helpers of the bench firmware without ESP-IDF dependencies
 * (argument parsing, input checks, masking, LED blink patterns, WiFi list
 * merging). Unit-tested on the host: firmware/tools/host_test_bench.sh.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"   /* settings_wifi_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- tone command ---------------------------------------------------------- */
#define BENCH_TONE_DEFAULT_FREQ_HZ      1000.0f
#define BENCH_TONE_DEFAULT_DBFS         (-20.0f)
#define BENCH_TONE_DEFAULT_SECONDS      5.0f
#define BENCH_NOISE_TEST_FREQ_HZ        1000.0f
#define BENCH_NOISE_TEST_DBFS           (-60.0f)
#define BENCH_NOISE_TEST_SECONDS        60.0f
#define BENCH_TONE_MIN_FREQ_HZ          10.0f
#define BENCH_TONE_MAX_FREQ_HZ          20000.0f
#define BENCH_TONE_MIN_DBFS             (-100.0f)
#define BENCH_TONE_MAX_SECONDS          3600.0f

typedef struct {
    float freq_hz;
    float dbfs;
    float seconds;
} bench_tone_args_t;

/**
 * Parse the arguments after "tone": [freq_hz] [dbfs] [seconds]. Missing values
 * get the defaults. dBFS may be written with or without the minus sign
 * ("20" = "-20"), a positive sign is rejected. Returns NULL on success, else
 * an English error message (static string).
 */
const char *bench_parse_tone(int argc, char **argv, bench_tone_args_t *out);

/** Strict number parser: the whole string must be a finite number. */
bool bench_parse_float(const char *s, float *out);

/* ---- input checks: NULL = valid, else an English error message ------------- */
const char *bench_check_ssid(const char *ssid);
/** "" (open network), 8..63 characters passphrase or 64 hex digits. */
const char *bench_check_wifi_password(const char *password);
/** 1..63 bytes (mDNS instance name limit), printable, no leading/trailing space. */
const char *bench_check_device_name(const char *name);
/** 1..64 printable characters without spaces. */
const char *bench_check_credential(const char *value);
/** Spotify client ids and secrets are 32 hex digits. */
bool bench_is_hex32(const char *s);

/** "****abcd": only the last 4 characters are shown (fewer for short input). */
void bench_mask_secret(const char *secret, char *out, size_t cap);

/** Joins argv[0..argc-1] with single spaces. False if it does not fit. */
bool bench_join_args(int argc, char **argv, char *out, size_t cap);

/* ---- status LED ------------------------------------------------------------- */
typedef enum {
    BENCH_LED_NO_WIFI,      /* slow blink: 1 Hz            */
    BENCH_LED_CONNECTED,    /* steady on                   */
    BENCH_LED_PLAYING,      /* fast blink: 5 Hz            */
} bench_led_mode_t;

/** Whether the LED is lit `ms` milliseconds into the pattern. */
bool bench_led_is_on(bench_led_mode_t mode, uint32_t ms);

/* ---- WiFi candidates ---------------------------------------------------------- */
/**
 * Networks to try, in order: the `n_stored` stored ones (as given), then the
 * build-time network `fallback_ssid` unless it is empty or already stored.
 * @return number of entries written to out (at most max).
 */
int bench_merge_wifi(const settings_wifi_t *stored, int n_stored, const char *fallback_ssid,
                     const char *fallback_password, settings_wifi_t *out, int max);

#ifdef __cplusplus
}
#endif
