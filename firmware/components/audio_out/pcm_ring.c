#include "pcm_ring.h"

#include <string.h>

static inline uint32_t load_acq(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store_rel(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void pcm_ring_init(pcm_ring_t *r, void *storage, size_t bytes)
{
    uint32_t size = 1;
    while ((size_t)size * 2 <= bytes && size < 0x80000000u) {
        size *= 2;
    }
    r->buf = (uint8_t *)storage;
    r->size = bytes > 0 ? size : 0;
    r->wr = 0;
    r->rd = 0;
}

size_t pcm_ring_used(const pcm_ring_t *r)
{
    /* read rd first: wr can only grow meanwhile, so the result never exceeds size */
    uint32_t rd = load_acq(&r->rd);
    uint32_t wr = load_acq(&r->wr);
    return (size_t)(wr - rd);
}

size_t pcm_ring_free(const pcm_ring_t *r)
{
    uint32_t wr = load_acq(&r->wr);
    uint32_t rd = load_acq(&r->rd);
    return (size_t)(r->size - (wr - rd));
}

size_t pcm_ring_write(pcm_ring_t *r, const void *src, size_t n)
{
    uint32_t wr = r->wr;                        /* own counter */
    uint32_t rd = load_acq(&r->rd);
    size_t space = r->size - (wr - rd);
    if (n > space) {
        n = space;
    }
    if (n == 0) {
        return 0;
    }
    uint32_t pos = wr & (r->size - 1);
    size_t first = r->size - pos;
    if (first > n) {
        first = n;
    }
    memcpy(r->buf + pos, src, first);
    if (n > first) {
        memcpy(r->buf, (const uint8_t *)src + first, n - first);
    }
    store_rel(&r->wr, wr + (uint32_t)n);        /* publish after the data */
    return n;
}

size_t pcm_ring_read(pcm_ring_t *r, void *dst, size_t n)
{
    uint32_t rd = r->rd;                        /* own counter */
    uint32_t wr = load_acq(&r->wr);
    size_t avail = wr - rd;
    if (n > avail) {
        n = avail;
    }
    if (n == 0) {
        return 0;
    }
    uint32_t pos = rd & (r->size - 1);
    size_t first = r->size - pos;
    if (first > n) {
        first = n;
    }
    memcpy(dst, r->buf + pos, first);
    if (n > first) {
        memcpy((uint8_t *)dst + first, r->buf, n - first);
    }
    store_rel(&r->rd, rd + (uint32_t)n);        /* hand the space back after copying */
    return n;
}

size_t pcm_ring_drop(pcm_ring_t *r)
{
    uint32_t wr = load_acq(&r->wr);
    uint32_t dropped = wr - r->rd;
    store_rel(&r->rd, wr);
    return dropped;
}
