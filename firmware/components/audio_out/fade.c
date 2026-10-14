#include "fade.h"

void fade_in_s16_at(int16_t *frames, size_t n_frames, size_t start, size_t fade_frames)
{
    if (start >= fade_frames) {
        return;
    }
    size_t n = fade_frames - start;
    if (n > n_frames) {
        n = n_frames;
    }
    const int32_t den = (int32_t)(fade_frames + 1);
    for (size_t i = 0; i < n; i++) {
        /* gain (k + 1) / (fade_frames + 1): never exactly 0, reaches ~1 at the end */
        int32_t num = (int32_t)(start + i + 1);
        frames[2 * i] = (int16_t)((int32_t)frames[2 * i] * num / den);
        frames[2 * i + 1] = (int16_t)((int32_t)frames[2 * i + 1] * num / den);
    }
}

void fade_in_s16(int16_t *frames, size_t n_frames, size_t fade_frames)
{
    fade_in_s16_at(frames, n_frames, 0, fade_frames);
}

void fade_out_ramp_s16_at(int16_t last_l, int16_t last_r, int16_t *out, size_t start, size_t count,
                          size_t fade_frames)
{
    const int32_t den = (int32_t)fade_frames;
    for (size_t i = 0; i < count && start + i < fade_frames; i++) {
        /* gain (fade_frames - 1 - k) / fade_frames: starts just below 1, ends at 0 */
        int32_t num = (int32_t)(fade_frames - 1 - (start + i));
        out[2 * i] = (int16_t)((int32_t)last_l * num / den);
        out[2 * i + 1] = (int16_t)((int32_t)last_r * num / den);
    }
}

void fade_out_ramp_s16(int16_t last_l, int16_t last_r, int16_t *out, size_t fade_frames)
{
    fade_out_ramp_s16_at(last_l, last_r, out, 0, fade_frames, fade_frames);
}
