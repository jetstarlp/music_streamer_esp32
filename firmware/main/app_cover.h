/*
 * app_cover - downloads a cover image (https://i.scdn.co/image/..., JPEG) and
 * decodes it with the ROM JPEG decoder into a UI_COVER_SIZE x UI_COVER_SIZE
 * RGB565 buffer in PSRAM (centre crop, nearest-neighbour scaling).
 *
 * One worker task; a new request replaces a pending one. The result is handed
 * to the callback on the worker task; the receiver owns the buffer and frees
 * it with heap_caps_free(). On failure the callback gets NULL.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_cover_cb_t)(uint32_t request_id, uint16_t *rgb565, uint16_t size, void *ctx);

esp_err_t app_cover_start(app_cover_cb_t cb, void *ctx);

/** Ask for a cover; request_id comes back in the callback. */
void app_cover_request(const char *url, uint32_t request_id);

#ifdef __cplusplus
}
#endif
