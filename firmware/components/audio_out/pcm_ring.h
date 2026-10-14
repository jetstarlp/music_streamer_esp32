/*
 * pcm_ring - single-producer / single-consumer byte ring for PCM audio.
 *
 * The producer (a task) and the consumer (the I2S DMA interrupt) run on
 * different cores without a common lock: the producer only ever moves `wr`,
 * the consumer only ever moves `rd`, both are free-running 32-bit counters
 * published with release/acquire ordering. The size is a power of two, so
 * counter wrap-around at 2^32 is harmless.
 *
 * pcm_ring_drop() is a consumer operation: whoever calls it must exclude the
 * consumer (audio_out does that with a spinlock shared with the interrupt).
 *
 * Pure C without ESP-IDF dependencies (host tests: tools/host_test_bench.sh).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  *buf;
    uint32_t  size;     /* power of two */
    uint32_t  wr;       /* bytes ever written (producer) */
    uint32_t  rd;       /* bytes ever consumed (consumer) */
} pcm_ring_t;

/** Use `storage` of `bytes` bytes; the usable size is rounded down to a power of two. */
void pcm_ring_init(pcm_ring_t *r, void *storage, size_t bytes);

/** Bytes waiting to be read (safe from either side). */
size_t pcm_ring_used(const pcm_ring_t *r);

/** Bytes that can be written right now (safe from either side). */
size_t pcm_ring_free(const pcm_ring_t *r);

/** Producer: copy up to `n` bytes in; returns how many were taken. */
size_t pcm_ring_write(pcm_ring_t *r, const void *src, size_t n);

/** Consumer: copy up to `n` bytes out; returns how many were read. */
size_t pcm_ring_read(pcm_ring_t *r, void *dst, size_t n);

/** Consumer: discard everything buffered; returns the number of bytes dropped. */
size_t pcm_ring_drop(pcm_ring_t *r);

#ifdef __cplusplus
}
#endif
