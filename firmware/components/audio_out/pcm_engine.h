/*
 * pcm_engine - decides what goes into each I2S DMA buffer.
 *
 * Called from the I2S "buffer sent" interrupt for every DMA buffer that has
 * to be refilled. Sources, in order: a running fade-out ramp, the test tone
 * ring, the music ring (unless paused or rebuffering), silence. Every
 * transition is click-free: real audio fades in over FADE_FRAMES after any
 * interruption, and whenever real audio stops (pause, skip, underrun, tone)
 * the output ramps from the last sample down to zero, also across buffer
 * boundaries.
 *
 * No locking inside: the caller serialises fill() against flush() and the
 * tone hand-over (audio_out uses one spinlock shared with the interrupt).
 * Pure C without ESP-IDF dependencies (host tests: tools/host_test_bench.sh).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pcm_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Unity gain in q16: music passes bit-exact (no multiplication at all). */
#define PCM_ENGINE_GAIN_UNITY 65536u

typedef struct {
    /* configuration */
    pcm_ring_t *music;
    pcm_ring_t *tone;               /* may be NULL */
    uint32_t    prebuffer_bytes;    /* after running dry: wait for this much audio ... */
    uint32_t    prebuffer_max_frames; /* ... but not longer than this many output frames */

    /* inputs, set by the owner between fills */
    bool        paused;
    bool        tone_mode;          /* play the tone ring instead of the music ring */
    bool        tone_eof;           /* the tone producer has written its last frame */
    uint32_t    gain_target;        /* music volume, q16 (PCM_ENGINE_GAIN_UNITY = 1.0) */

    /* state */
    uint32_t    gain_cur;           /* gain of the last music frame; ramps to gain_target */
    bool        rebuffering;
    uint32_t    rebuffer_waited;    /* output frames spent waiting with some audio buffered */
    bool        had_audio;          /* real audio since the last underrun (for counting) */
    bool        sounding;           /* last output frame was real audio, not yet ramped down */
    bool        ramping;            /* fade-out ramp in progress */
    uint32_t    ramp_pos;
    uint32_t    fade_in_pos;        /* frames of the current fade-in done (>= FADE_FRAMES: none) */
    int16_t     last_l, last_r;     /* last real-audio frame (start of the ramp) */

    /* outputs */
    bool        tone_finished;      /* set when tone mode ended by itself; owner clears it */
    uint64_t    frames_played;      /* real-audio frames put into DMA buffers */
    uint64_t    frames_dropped;     /* frames discarded by pcm_engine_flush() */
    uint32_t    underruns;          /* music ring ran dry while playing */
    uint32_t    music_bytes_read;   /* since the owner last cleared it (wake the producer) */
} pcm_engine_t;

void pcm_engine_init(pcm_engine_t *e, pcm_ring_t *music, pcm_ring_t *tone, uint32_t prebuffer_bytes,
                     uint32_t prebuffer_max_frames);

/** Fill `frames` stereo frames (16-bit, L/R interleaved). Always writes all of them. */
void pcm_engine_fill(pcm_engine_t *e, int16_t *out, size_t frames);

/**
 * Discard all buffered music (seek, skip). If audio was sounding it ramps
 * down in the next fill; new audio is prebuffered and faded in.
 */
void pcm_engine_flush(pcm_engine_t *e);

/**
 * Pretend the last output frame was (l, r) and ramp down from there in the
 * next fill. Used after a flush when the caller rewrites DMA buffers that were
 * already queued: the ramp then starts from what the DAC really plays last.
 * (pcm_engine_flush() itself ramps from the last frame written.)
 */
void pcm_engine_ramp_from(pcm_engine_t *e, int16_t l, int16_t r);

/** Forget all transient state (output stopped): silence, then fade-in. */
void pcm_engine_reset_output(pcm_engine_t *e);

#ifdef __cplusplus
}
#endif
