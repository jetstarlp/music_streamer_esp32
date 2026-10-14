#include "tone_gen.h"

#include <math.h>

#define TWO_PI_F        6.28318530718f
#define PHASE_TO_RAD    (TWO_PI_F / 4294967296.0f)
#define FULL_SCALE      32767.0f

float tone_gen_dbfs_to_peak(float dbfs)
{
    return FULL_SCALE * powf(10.0f, dbfs / 20.0f);
}

bool tone_gen_init(tone_gen_t *t, uint32_t sample_rate_hz, float freq_hz, float dbfs,
                   uint32_t duration_ms)
{
    if (t == NULL || sample_rate_hz < 8000 || sample_rate_hz > 192000 ||
        !(freq_hz > 0.0f) || !(freq_hz < (float)sample_rate_hz / 2.0f) ||
        !(dbfs >= TONE_GEN_MIN_DBFS) || !(dbfs <= TONE_GEN_MAX_DBFS) ||
        duration_ms == 0 || duration_ms > 3600u * 1000u) {
        return false;
    }
    tone_gen_t g = {0};
    g.phase_inc = (uint32_t)llround((double)freq_hz * 4294967296.0 / (double)sample_rate_hz);
    g.peak = tone_gen_dbfs_to_peak(dbfs);
    g.end = (uint32_t)(((uint64_t)duration_ms * sample_rate_hz) / 1000u);
    if (g.end == 0) {
        g.end = 1;
    }
    g.fade = (uint32_t)(((uint64_t)TONE_GEN_FADE_MS * sample_rate_hz) / 1000u);
    if (g.fade > g.end / 2) {
        g.fade = g.end / 2;
    }
    *t = g;
    return true;
}

/* Raised-cosine envelope: 0 at the edges, 1 in the middle. */
static float envelope(const tone_gen_t *t)
{
    if (t->fade == 0) {
        return 1.0f;
    }
    uint32_t from_start = t->pos;
    uint32_t to_end = t->end - t->pos;      /* >= 1 while producing */
    uint32_t d = from_start < to_end ? from_start : to_end;
    if (d >= t->fade) {
        return 1.0f;
    }
    float x = (float)d / (float)t->fade;
    return 0.5f - 0.5f * cosf(x * (TWO_PI_F / 2.0f));
}

size_t tone_gen_fill(tone_gen_t *t, int16_t *out, size_t frames)
{
    size_t n = 0;
    while (n < frames && t->pos < t->end) {
        float v = t->peak * envelope(t) * sinf((float)t->phase * PHASE_TO_RAD);
        long s = lrintf(v);
        if (s > 32767) {
            s = 32767;
        } else if (s < -32767) {
            s = -32767;
        }
        out[2 * n] = (int16_t)s;
        out[2 * n + 1] = (int16_t)s;
        t->phase += t->phase_inc;
        t->pos++;
        n++;
    }
    return n;
}

void tone_gen_stop(tone_gen_t *t)
{
    /* Shorten the tone so that it ends after one fade length. The envelope
     * uses the distance to the end, so the level ramps down from where it is. */
    if (t->pos < t->end && t->end - t->pos > t->fade) {
        t->end = t->pos + t->fade;
    }
}

bool tone_gen_done(const tone_gen_t *t)
{
    return t->pos >= t->end;
}
