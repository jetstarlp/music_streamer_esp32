/*
 * tone_gen - sine test-tone generator used by audio_out's feeder task.
 *
 * Pure C without ESP-IDF dependencies so that it can be unit-tested on the
 * host (firmware/tools/host_test_bench.sh). Output: 16-bit stereo frames with
 * the same signal on both channels, raised-cosine fade-in/-out of
 * TONE_GEN_FADE_MS to avoid clicks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TONE_GEN_FADE_MS    10
#define TONE_GEN_MIN_DBFS   (-100.0f)
#define TONE_GEN_MAX_DBFS   0.0f

typedef struct {
    uint32_t phase;         /* phase accumulator, 2^32 = one period            */
    uint32_t phase_inc;     /* per frame                                       */
    float    peak;          /* amplitude in LSB of a 16-bit sample             */
    uint32_t pos;           /* frames produced so far                          */
    uint32_t end;           /* total number of frames including the fade-out   */
    uint32_t fade;          /* fade-in/-out length in frames                   */
} tone_gen_t;

/** Peak amplitude in LSB for a sine of `dbfs` dB relative to full scale (0 dBFS = 32767). */
float tone_gen_dbfs_to_peak(float dbfs);

/**
 * Prepare a tone. Returns false (and leaves *t untouched) when a parameter is
 * out of range: sample rate 8000..192000 Hz, 0 < freq_hz < sample_rate / 2,
 * TONE_GEN_MIN_DBFS <= dbfs <= TONE_GEN_MAX_DBFS, duration_ms 1..3600000.
 */
bool tone_gen_init(tone_gen_t *t, uint32_t sample_rate_hz, float freq_hz, float dbfs,
                   uint32_t duration_ms);

/**
 * Write up to `frames` stereo frames (L, R interleaved) to `out`.
 * @return number of frames written; 0 once the tone has ended.
 */
size_t tone_gen_fill(tone_gen_t *t, int16_t *out, size_t frames);

/** End the tone early with a fade-out (no click). */
void tone_gen_stop(tone_gen_t *t);

bool tone_gen_done(const tone_gen_t *t);

#ifdef __cplusplus
}
#endif
