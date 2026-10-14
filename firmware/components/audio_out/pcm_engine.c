#include "pcm_engine.h"

#include <string.h>

#include "fade.h"

#define BYTES_PER_FRAME 4u

void pcm_engine_init(pcm_engine_t *e, pcm_ring_t *music, pcm_ring_t *tone, uint32_t prebuffer_bytes,
                     uint32_t prebuffer_max_frames)
{
    memset(e, 0, sizeof(*e));
    e->music = music;
    e->tone = tone;
    e->prebuffer_bytes = prebuffer_bytes;
    e->prebuffer_max_frames = prebuffer_max_frames;
    e->rebuffering = prebuffer_bytes > 0;
    e->gain_cur = e->gain_target = PCM_ENGINE_GAIN_UNITY;
}

static void start_ramp(pcm_engine_t *e)
{
    e->ramping = true;
    e->ramp_pos = 0;
    e->sounding = false;
}

/* Music source: returns frames read (0 = nothing to play right now). */
static size_t read_music(pcm_engine_t *e, int16_t *out, size_t want)
{
    if (e->rebuffering) {
        size_t avail = pcm_ring_used(e->music);
        if (avail == 0) {
            e->rebuffer_waited = 0;
            return 0;
        }
        if (avail < e->prebuffer_bytes && e->rebuffer_waited < e->prebuffer_max_frames) {
            return 0;
        }
        e->rebuffering = false;
    }
    size_t got = pcm_ring_read(e->music, out, want * BYTES_PER_FRAME) / BYTES_PER_FRAME;
    if (got == 0) {
        /* ran dry while playing */
        if (e->had_audio) {
            e->underruns++;
            e->had_audio = false;
        }
        e->rebuffering = e->prebuffer_bytes > 0;
        e->rebuffer_waited = 0;
        return 0;
    }
    e->music_bytes_read += (uint32_t)(got * BYTES_PER_FRAME);
    /* volume: q16 gain, ramped linearly over this chunk when it changed */
    if (e->gain_cur != PCM_ENGINE_GAIN_UNITY || e->gain_target != PCM_ENGINE_GAIN_UNITY) {
        int64_t g0 = e->gain_cur, g1 = e->gain_target;
        for (size_t i = 0; i < got; i++) {
            int32_t g = (int32_t)(g0 + (g1 - g0) * (int64_t)(i + 1) / (int64_t)got);
            out[2 * i] = (int16_t)(((int32_t)out[2 * i] * g) >> 16);
            out[2 * i + 1] = (int16_t)(((int32_t)out[2 * i + 1] * g) >> 16);
        }
        e->gain_cur = e->gain_target;
    }
    if (e->fade_in_pos < FADE_FRAMES) {
        fade_in_s16_at(out, got, e->fade_in_pos, FADE_FRAMES);
        e->fade_in_pos += (uint32_t)(got < FADE_FRAMES ? got : FADE_FRAMES);
    }
    e->last_l = out[2 * (got - 1)];
    e->last_r = out[2 * (got - 1) + 1];
    e->sounding = true;
    e->had_audio = true;
    e->frames_played += got;
    return got;
}

void pcm_engine_fill(pcm_engine_t *e, int16_t *out, size_t frames)
{
    size_t pos = 0;
    while (pos < frames) {
        int16_t *d = out + 2 * pos;
        size_t want = frames - pos;

        if (e->ramping) {
            size_t left = FADE_FRAMES - e->ramp_pos;
            size_t n = want < left ? want : left;
            fade_out_ramp_s16_at(e->last_l, e->last_r, d, e->ramp_pos, n, FADE_FRAMES);
            e->ramp_pos += (uint32_t)n;
            pos += n;
            if (e->ramp_pos >= FADE_FRAMES) {
                e->ramping = false;
                e->fade_in_pos = 0;         /* whatever real audio comes next fades in */
            }
            continue;
        }

        if (e->tone_mode && e->tone) {
            if (e->sounding) {
                start_ramp(e);              /* music first ramps down */
                continue;
            }
            size_t got = pcm_ring_read(e->tone, d, want * BYTES_PER_FRAME) / BYTES_PER_FRAME;
            if (got > 0) {
                pos += got;                 /* the tone has its own fade-in/-out */
                continue;
            }
            if (e->tone_eof) {
                e->tone_mode = false;
                e->tone_finished = true;
                e->fade_in_pos = 0;
                continue;                   /* music (if any) takes over in this same buffer */
            }
            memset(d, 0, want * BYTES_PER_FRAME);   /* tone producer late */
            pos = frames;
            continue;
        }

        if (!e->paused) {
            size_t got = read_music(e, d, want);
            if (got > 0) {
                pos += got;
                continue;
            }
        }

        if (e->sounding) {
            start_ramp(e);
            continue;
        }
        memset(d, 0, want * BYTES_PER_FRAME);
        if (!e->paused && e->rebuffering) {
            e->rebuffer_waited += (uint32_t)want;
        }
        pos = frames;
    }
}

void pcm_engine_flush(pcm_engine_t *e)
{
    e->frames_dropped += pcm_ring_drop(e->music) / BYTES_PER_FRAME;
    e->rebuffering = e->prebuffer_bytes > 0;
    e->rebuffer_waited = 0;
    e->had_audio = false;
    if (e->sounding) {
        start_ramp(e);      /* the new position must not follow the old audio directly */
    }
}

void pcm_engine_ramp_from(pcm_engine_t *e, int16_t l, int16_t r)
{
    e->last_l = l;
    e->last_r = r;
    start_ramp(e);
}

void pcm_engine_reset_output(pcm_engine_t *e)
{
    e->sounding = false;
    e->ramping = false;
    e->ramp_pos = 0;
    e->fade_in_pos = 0;
    e->last_l = 0;
    e->last_r = 0;
    e->tone_mode = false;
}
