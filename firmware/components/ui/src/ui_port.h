/*
 * ui_port.h - the few platform services the UI needs besides LVGL: a mutex
 * and an allocator for the cover copy. Both are plain POSIX/libc, available
 * on ESP-IDF (pthread on FreeRTOS) and on the PC. No ESP-IDF API here.
 *
 * Allocation: on the device malloc() puts blocks above
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 kB in this firmware) into PSRAM, so
 * the 64 kB cover copy never touches internal RAM.
 */
#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

static inline void *ui_port_alloc_large(size_t n)
{
    return malloc(n);
}

static inline void ui_port_free(void *p)
{
    free(p);
}

typedef pthread_mutex_t ui_mutex_t;
#define UI_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void ui_mutex_lock(ui_mutex_t *m)
{
    pthread_mutex_lock(m);
}
static inline void ui_mutex_unlock(ui_mutex_t *m)
{
    pthread_mutex_unlock(m);
}

/* Optional logging through LVGL's logger (LV_USE_LOG); define UI_NO_LOG to drop it. */
#ifndef UI_NO_LOG
#define UI_LOG(...) LV_LOG_USER(__VA_ARGS__)
#else
#define UI_LOG(...) ((void)0)
#endif
