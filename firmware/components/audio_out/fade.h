/*
 * fade - short linear fades for 16-bit stereo PCM, used by audio_out's feeder
 * so that audio never starts or stops with a jump (pause, skip, seek, buffer
 * underrun). Pure C, unit-tested on the host (tools/host_test_bench.sh).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 256 frames = 5.8 ms at 44.1 kHz: inaudible as a fade, long enough to avoid a click. */
#define FADE_FRAMES 256

/** Scale the first min(n_frames, fade_frames) stereo frames linearly from ~0 up to 1. */
void fade_in_s16(int16_t *frames, size_t n_frames, size_t fade_frames);

/**
 * Write `fade_frames` stereo frames that ramp linearly from the last output
 * sample (last_l, last_r) down to 0 (the final frame is exactly 0).
 */
void fade_out_ramp_s16(int16_t last_l, int16_t last_r, int16_t *out, size_t fade_frames);

/**
 * Same as fade_in_s16(), but `frames` continues a fade that has already
 * covered `start` frames (a fade split across several buffers).
 */
void fade_in_s16_at(int16_t *frames, size_t n_frames, size_t start, size_t fade_frames);

/**
 * Frames start .. start + count - 1 of the ramp of fade_out_ramp_s16() (only
 * those below fade_frames are written).
 */
void fade_out_ramp_s16_at(int16_t last_l, int16_t last_r, int16_t *out, size_t start, size_t count,
                          size_t fade_frames);

#ifdef __cplusplus
}
#endif
