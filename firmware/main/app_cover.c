/*
 * app_cover - see app_cover.h.
 */
#include "app_cover.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"
#include "ui_model.h"

static const char *TAG = "cover";

#define COVER_MAX_JPEG      (400 * 1024)
#define COVER_URL_MAX       160
#define COVER_WORK_SIZE     4096        /* tjpgd needs ~3.1 kB */
#define COVER_TASK_STACK    (8 * 1024)  /* TLS handshake; stack in PSRAM */

static struct {
    app_cover_cb_t    cb;
    void             *ctx;
    TaskHandle_t      task;
    SemaphoreHandle_t lock;
    char              url[COVER_URL_MAX];   /* pending request, "" = none */
    uint32_t          id;
} s_cov;

/* ---- download -------------------------------------------------------------------- */

static uint8_t *download(const char *url, size_t *len_out)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return NULL;
    }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        goto out;
    }
    int64_t content_len = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        goto out;
    }
    cap = content_len > 0 && content_len <= COVER_MAX_JPEG ? (size_t)content_len : 64 * 1024;
    buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        goto out;
    }
    for (;;) {
        if (len == cap) {
            if (cap >= COVER_MAX_JPEG) {
                ESP_LOGW(TAG, "image too large");
                goto fail;
            }
            size_t ncap = cap * 2 > COVER_MAX_JPEG ? COVER_MAX_JPEG : cap * 2;
            uint8_t *n = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (n == NULL) {
                goto fail;
            }
            buf = n;
            cap = ncap;
        }
        int r = esp_http_client_read(c, (char *)buf + len, (int)(cap - len));
        if (r < 0) {
            goto fail;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    *len_out = len;
    goto out;
fail:
    heap_caps_free(buf);
    buf = NULL;
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

/* ---- decode ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t *jpeg;
    size_t         len, pos;
    uint16_t      *img;     /* decoded (scaled) image, RGB565 */
    uint16_t       w, h;
} dec_ctx_t;

static UINT dec_in(JDEC *jd, BYTE *buf, UINT n)
{
    dec_ctx_t *d = (dec_ctx_t *)jd->device;
    size_t left = d->len - d->pos;
    if (n > left) {
        n = (UINT)left;
    }
    if (buf) {
        memcpy(buf, d->jpeg + d->pos, n);
    }
    d->pos += n;
    return n;
}

static UINT dec_out(JDEC *jd, void *bitmap, JRECT *r)
{
    dec_ctx_t *d = (dec_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;    /* RGB888 */
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++) {
            uint8_t R = *src++, G = *src++, B = *src++;
            if (x < d->w && y < d->h) {
                d->img[y * d->w + x] = (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
            }
        }
    }
    return 1;
}

static uint16_t *decode(const uint8_t *jpeg, size_t len)
{
    JDEC jd;
    dec_ctx_t d = {.jpeg = jpeg, .len = len};
    void *work = heap_caps_malloc(COVER_WORK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *out = NULL;
    if (work == NULL) {
        return NULL;
    }
    JRESULT rc = jd_prepare(&jd, dec_in, work, COVER_WORK_SIZE, &d);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare: %d (progressive JPEG?)", (int)rc);
        goto done;
    }
    BYTE scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= UI_COVER_SIZE &&
           (jd.height >> (scale + 1)) >= UI_COVER_SIZE) {
        scale++;
    }
    d.w = (uint16_t)(jd.width >> scale);
    d.h = (uint16_t)(jd.height >> scale);
    if (d.w == 0 || d.h == 0) {
        goto done;
    }
    d.img = heap_caps_calloc((size_t)d.w * d.h, 2, MALLOC_CAP_SPIRAM);
    if (d.img == NULL) {
        goto done;
    }
    rc = jd_decomp(&jd, dec_out, scale);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp: %d", (int)rc);
        goto done;
    }
    /* centre crop to a square, nearest neighbour to UI_COVER_SIZE */
    out = heap_caps_malloc(UI_COVER_SIZE * UI_COVER_SIZE * 2, MALLOC_CAP_SPIRAM);
    if (out) {
        uint16_t side = d.w < d.h ? d.w : d.h;
        uint16_t ox = (d.w - side) / 2, oy = (d.h - side) / 2;
        for (int y = 0; y < UI_COVER_SIZE; y++) {
            int sy = oy + y * side / UI_COVER_SIZE;
            const uint16_t *row = d.img + (size_t)sy * d.w;
            for (int x = 0; x < UI_COVER_SIZE; x++) {
                out[y * UI_COVER_SIZE + x] = row[ox + x * side / UI_COVER_SIZE];
            }
        }
    }
done:
    heap_caps_free(d.img);
    heap_caps_free(work);
    return out;
}

/* ---- worker --------------------------------------------------------------------- */

/* The cover is requested just when a new track starts, which is also when the
 * audio stream needs the (often weak) WiFi most. Let the audio buffer reach
 * about one second first, but do not hold the cover back longer than 4 s. */
#define COVER_AUDIO_AHEAD_BYTES  (44100u * 4u)
#define COVER_WAIT_MAX_MS        4000

static void wait_for_audio_buffer(void)
{
    for (int waited = 0; waited < COVER_WAIT_MAX_MS; waited += 100) {
        audio_out_stats_t st;
        audio_out_get_stats(&st);
        if (!st.running || st.paused || st.buffered_bytes >= COVER_AUDIO_AHEAD_BYTES) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void cover_task(void *arg)
{
    (void)arg;
    char url[COVER_URL_MAX];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t id;
        xSemaphoreTake(s_cov.lock, portMAX_DELAY);
        strlcpy(url, s_cov.url, sizeof(url));
        id = s_cov.id;
        s_cov.url[0] = '\0';
        xSemaphoreGive(s_cov.lock);
        if (url[0] == '\0') {
            continue;
        }
        wait_for_audio_buffer();
        xSemaphoreTake(s_cov.lock, portMAX_DELAY);
        bool superseded = s_cov.url[0] != '\0';   /* a newer track came meanwhile */
        xSemaphoreGive(s_cov.lock);
        if (superseded) {
            xTaskNotifyGive(s_cov.task);
            continue;
        }
        size_t len = 0;
        uint16_t *img = NULL;
        uint8_t *jpeg = download(url, &len);
        if (jpeg) {
            ESP_LOGI(TAG, "downloaded %u bytes", (unsigned)len);
            img = decode(jpeg, len);
            heap_caps_free(jpeg);
        }
        if (img == NULL) {
            ESP_LOGW(TAG, "no cover from %s", url);
        }
        /* a newer request may be pending: deliver anyway, the receiver
         * compares the id */
        s_cov.cb(id, img, img ? UI_COVER_SIZE : 0, s_cov.ctx);
    }
}

esp_err_t app_cover_start(app_cover_cb_t cb, void *ctx)
{
    if (s_cov.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cov.cb = cb;
    s_cov.ctx = ctx;
    s_cov.lock = xSemaphoreCreateMutex();
    if (s_cov.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(cover_task, "cover", COVER_TASK_STACK, NULL, 2, &s_cov.task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_cover_request(const char *url, uint32_t request_id)
{
    if (s_cov.task == NULL || url == NULL) {
        return;
    }
    xSemaphoreTake(s_cov.lock, portMAX_DELAY);
    strlcpy(s_cov.url, url, sizeof(s_cov.url));
    s_cov.id = request_id;
    xSemaphoreGive(s_cov.lock);
    xTaskNotifyGive(s_cov.task);
}
