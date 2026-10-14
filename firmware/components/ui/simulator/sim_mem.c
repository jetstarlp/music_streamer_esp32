/*
 * LVGL allocator of the simulator (LV_STDLIB_CUSTOM): plain malloc with a
 * size header, counting live blocks the way the ESP32 heap would split them
 * (blocks up to 4096 bytes stay in internal RAM, larger ones go to PSRAM,
 * see CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). Host sizes are larger than on
 * the 32-bit target (8-byte pointers), so the numbers are an upper bound.
 */
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "sim_mem.h"

#define HDR 16
#define INTERNAL_MAX 4096

sim_mem_stats_t g_sim_mem;

static void account(size_t n, int sign)
{
    if (n <= INTERNAL_MAX) {
        g_sim_mem.small_bytes += sign * (long)n;
        g_sim_mem.small_blocks += sign;
        if (g_sim_mem.small_bytes > g_sim_mem.small_peak) {
            g_sim_mem.small_peak = g_sim_mem.small_bytes;
        }
    }
    else {
        g_sim_mem.large_bytes += sign * (long)n;
    }
}

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    unsigned char *p = malloc(size + HDR);
    if (!p) {
        return NULL;
    }
    memcpy(p, &size, sizeof(size));
    account(size, 1);
    return p + HDR;
}

void lv_free_core(void *ptr)
{
    if (!ptr) {
        return;
    }
    unsigned char *p = (unsigned char *)ptr - HDR;
    size_t size;
    memcpy(&size, p, sizeof(size));
    account(size, -1);
    free(p);
}

void *lv_realloc_core(void *ptr, size_t new_size)
{
    if (!ptr) {
        return lv_malloc_core(new_size);
    }
    unsigned char *p = (unsigned char *)ptr - HDR;
    size_t old;
    memcpy(&old, p, sizeof(old));
    unsigned char *q = realloc(p, new_size + HDR);
    if (!q) {
        return NULL;
    }
    account(old, -1);
    memcpy(q, &new_size, sizeof(new_size));
    account(new_size, 1);
    return q + HDR;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    (void)mon_p;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
