/*
 * LVGL allocator (CONFIG_LV_USE_CUSTOM_MALLOC): everything in PSRAM, internal
 * RAM only as a fallback. With plain malloc() the ~900 small LVGL objects of
 * the UI (all below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) would take 25-30 kB of
 * the scarce internal RAM. Draw buffers are not affected: esp_lvgl_port
 * allocates them itself as DMA-capable internal memory.
 */
#include "sdkconfig.h"

#if CONFIG_LV_USE_CUSTOM_MALLOC

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_PSRAM_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_FALLBACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, LV_PSRAM_CAPS, LV_FALLBACK_CAPS);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, LV_PSRAM_CAPS, LV_FALLBACK_CAPS);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif /* CONFIG_LV_USE_CUSTOM_MALLOC */
