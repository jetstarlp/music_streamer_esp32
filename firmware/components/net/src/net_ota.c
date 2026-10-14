/*
 * OTA updates from a JSON manifest, see include/net_ota.h.
 *
 * Structure: an esp_timer ("net_ota", every 30 s) decides when something is
 * due and creates the worker task, which runs one job (check, install,
 * restart) and deletes itself. The worker needs an internal-RAM stack because
 * it writes to flash (a task with a PSRAM stack must not). Download buffers
 * are in PSRAM, mbedTLS allocates there as well (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC).
 */
#include "net_ota.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "net_internal.h"
#include "net_pure.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "net_ota";

#define WORKER_STACK        8192
#define WORKER_PRIO         2
#define TICK_US             (30LL * 1000000)
#define RETRY_FAILED_US     (3600LL * 1000000)
#define HTTP_TIMEOUT_MS     20000
#define HTTP_BUF_RX         2048
#define HTTP_BUF_TX         2048    /* request line incl. the long signed GitHub asset URL */
#define VERIFY_CHUNK        4096
#define REBOOT_DELAY_MS     1500

#if CONFIG_MS_OTA_TEST_CA
/* QEMU test build only: trust the test CA of tools/qemu_ota_test.py instead
 * of the public CA bundle (Kconfig: depends on MS_NET_QEMU_OPENETH). */
extern const char ota_test_ca_pem_start[] asm("_binary_ota_test_ca_pem_start");
#define TLS_CONFIG .cert_pem = ota_test_ca_pem_start
#else
#define TLS_CONFIG .crt_bundle_attach = esp_crt_bundle_attach
#endif

#if CONFIG_MS_OTA_AUTO_INSTALL
#define AUTO_INSTALL true
#else
#define AUTO_INSTALL false
#endif

typedef enum {
    JOB_AUTO = 0,       /* scheduled: check (install only with AUTO_INSTALL)  */
    JOB_CHECK,          /* explicit: check only                                */
    JOB_INSTALL,        /* explicit: check and install if newer                */
    JOB_REBOOT,         /* installed: restart now                              */
    JOB_ROLLBACK,       /* not confirmed in time: back to the previous image   */
} job_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static net_ota_status_t s_st;           /* guarded by s_mux */
static bool s_started;
static TaskHandle_t s_worker;           /* non-NULL while a job runs */
static esp_timer_handle_t s_tick;
static esp_timer_handle_t s_verify_timer;
static bool (*s_busy)(void);
static net_ota_event_cb_t s_cb;
static void *s_cb_ctx;
static char s_default_url[NET_URL_MAX];
static char s_bad_version[NET_VERSION_MAX];  /* rolled back once: no automatic retry */
static int64_t s_up_since_us;           /* uplink up since (0 = down) */
static int64_t s_last_ok_us;            /* last successful manifest check, 0 = never */
static int64_t s_last_try_us;           /* last check attempt, 0 = never */
static bool s_last_failed;
static bool s_auto_pending;             /* AUTO_INSTALL: newer version waits for an idle moment */

static esp_err_t spawn(job_t job);

/* ---- status ---------------------------------------------------------------------- */

const char *net_ota_state_name(net_ota_state_t state)
{
    switch (state) {
    case NET_OTA_DISABLED:    return "disabled (no manifest URL)";
    case NET_OTA_IDLE:        return "idle";
    case NET_OTA_CHECKING:    return "checking";
    case NET_OTA_UP_TO_DATE:  return "up to date";
    case NET_OTA_AVAILABLE:   return "update available";
    case NET_OTA_DOWNLOADING: return "downloading";
    case NET_OTA_VERIFYING:   return "verifying";
    case NET_OTA_READY:       return "installed, restarts when nothing plays";
    case NET_OTA_REBOOTING:   return "restarting";
    case NET_OTA_FAILED:      return "failed";
    }
    return "?";
}

static void notify(void)
{
    net_ota_status_t copy;
    net_ota_get_status(&copy);
    net_ota_event_cb_t cb = s_cb;
    if (cb) {
        cb(&copy, s_cb_ctx);
    }
}

static void set_state(net_ota_state_t state)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = state;
    if (state != NET_OTA_DOWNLOADING) {
        s_st.progress = state == NET_OTA_READY ? 100 : 0;
    }
    if (state != NET_OTA_FAILED) {
        s_st.last_error[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
    notify();
}

static net_ota_state_t get_state(void)
{
    portENTER_CRITICAL(&s_mux);
    net_ota_state_t st = s_st.state;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    char msg[sizeof(s_st.last_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", msg);
    portENTER_CRITICAL(&s_mux);
    s_st.state = NET_OTA_FAILED;
    s_st.progress = 0;
    strlcpy(s_st.last_error, msg, sizeof(s_st.last_error));
    portEXIT_CRITICAL(&s_mux);
    notify();
}

void net_ota_get_status(net_ota_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    int64_t last_ok = s_last_ok_us;
    portEXIT_CRITICAL(&s_mux);
    out->last_check_age_s = last_ok ? (uint32_t)((esp_timer_get_time() - last_ok) / 1000000) : UINT32_MAX;
}

void net_ota_set_event_cb(net_ota_event_cb_t cb, void *ctx)
{
    s_cb_ctx = ctx;
    s_cb = cb;
}

void net_ota_set_busy_cb(bool (*busy)(void))
{
    s_busy = busy;
}

static bool busy(void)
{
    bool (*b)(void) = s_busy;
    return b != NULL && b();
}

bool net_ota_pending_verify(void)
{
    return s_st.pending_verify;
}

/* ---- URL -------------------------------------------------------------------------- */

void net_ota_get_url(char *out, size_t cap, bool *from_nvs)
{
    bool nvs = settings_get_str(SETTINGS_KEY_OTA_URL, out, cap) == ESP_OK && out[0] != '\0';
    if (!nvs) {
        strlcpy(out, s_default_url, cap);
    }
    if (from_nvs) {
        *from_nvs = nvs;
    }
}

esp_err_t net_ota_set_url(const char *url)
{
    esp_err_t err;
    if (url == NULL || url[0] == '\0') {
        err = settings_erase(SETTINGS_KEY_OTA_URL);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        if (!net_url_valid(url, false, NET_URL_MAX)) {
            return ESP_ERR_INVALID_ARG;
        }
        err = settings_set_str(SETTINGS_KEY_OTA_URL, url);
    }
    if (err != ESP_OK) {
        return err;
    }
    char cur[NET_URL_MAX];
    net_ota_get_url(cur, sizeof(cur), NULL);
    bool idle;
    portENTER_CRITICAL(&s_mux);
    s_last_try_us = 0;          /* check the new source soon */
    s_last_failed = false;
    s_auto_pending = false;
    s_st.available_version[0] = '\0';
    s_st.notes[0] = '\0';
    idle = s_worker == NULL && s_st.state != NET_OTA_READY && s_st.state != NET_OTA_REBOOTING;
    portEXIT_CRITICAL(&s_mux);
    if (idle) {
        set_state(cur[0] ? NET_OTA_IDLE : NET_OTA_DISABLED);
    }
    return ESP_OK;
}

/* ---- network ---------------------------------------------------------------------- */

/* WiFi station or (QEMU test build) Ethernet has an address. Not the setup
 * hotspot: it has no internet. */
static bool uplink_up(void)
{
    static const char *const keys[] = {"WIFI_STA_DEF", "ETH_DEF"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(keys[i]);
        esp_netif_ip_info_t ip;
        if (nif && esp_netif_is_netif_up(nif) && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   too_big;
} fetch_ctx_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(evt->client) == 200) {
        if (ctx->len + (size_t)evt->data_len >= ctx->cap) {
            ctx->too_big = true;
        } else {
            memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
            ctx->len += (size_t)evt->data_len;
        }
    }
    return ESP_OK;
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* Download and parse the manifest; on failure fill `err`. */
static bool fetch_manifest(const char *url, net_manifest_t *m, char *err, size_t err_cap)
{
    fetch_ctx_t ctx = {.cap = NET_MANIFEST_MAX + 1};
    ctx.buf = psram_alloc(ctx.cap);
    if (ctx.buf == NULL) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        TLS_CONFIG,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_BUF_RX,
        .buffer_size_tx = HTTP_BUF_TX,
        .event_handler = on_http,
        .user_data = &ctx,
        .max_redirection_count = 5,
    };
    bool ok = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        snprintf(err, err_cap, "manifest: HTTP client init failed");
    } else {
        esp_err_t e = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (e != ESP_OK) {
            snprintf(err, err_cap, "manifest download failed: %s", esp_err_to_name(e));
        } else if (status != 200) {
            snprintf(err, err_cap, "manifest download failed: HTTP %d", status);
        } else if (ctx.too_big) {
            snprintf(err, err_cap, "manifest larger than %d bytes", NET_MANIFEST_MAX);
        } else {
            const char *why = net_manifest_parse(ctx.buf, ctx.len, url, false, m);
            if (why) {
                snprintf(err, err_cap, "manifest: %s", why);
            } else {
                ok = true;
            }
        }
        esp_http_client_cleanup(client);
    }
    free(ctx.buf);
    return ok;
}

/* ---- install ------------------------------------------------------------------------ */

/* SHA-256 over the first `len` bytes of `part`, as written to flash. */
static bool partition_sha_matches(const esp_partition_t *part, size_t len, const uint8_t want[32])
{
    uint8_t *buf = heap_caps_malloc(VERIFY_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(VERIFY_CHUNK);
    }
    if (buf == NULL) {
        return false;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
    for (size_t off = 0; ok && off < len; off += VERIFY_CHUNK) {
        size_t n = len - off < VERIFY_CHUNK ? len - off : VERIFY_CHUNK;
        ok = esp_partition_read(part, off, buf, n) == ESP_OK && mbedtls_sha256_update(&ctx, buf, n) == 0;
    }
    uint8_t got[32];
    ok = ok && mbedtls_sha256_finish(&ctx, got) == 0 && memcmp(got, want, sizeof(got)) == 0;
    mbedtls_sha256_free(&ctx);
    free(buf);
    return ok;
}

static void do_reboot(void)
{
    ESP_LOGW(TAG, "restarting into the new firmware");
    set_state(NET_OTA_REBOOTING);           /* the application mutes the audio output */
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

/* Returns true if installed (state READY). `deferrable`: abort when busy. */
static bool install(const net_manifest_t *m, bool deferrable)
{
    const esp_app_desc_t *run = esp_app_get_description();
    ESP_LOGI(TAG, "installing %s from %s", m->version, m->url);
    portENTER_CRITICAL(&s_mux);
    s_st.progress = 0;
    portEXIT_CRITICAL(&s_mux);
    set_state(NET_OTA_DOWNLOADING);

    const esp_http_client_config_t http = {
        .url = m->url,
        TLS_CONFIG,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_BUF_RX,
        .buffer_size_tx = HTTP_BUF_TX,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        .buffer_caps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT,
    };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        fail("download of %s failed: %s", m->version, esp_err_to_name(err));
        return false;
    }
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(h, &desc);
    if (err != ESP_OK) {
        esp_https_ota_abort(h);
        fail("no valid firmware image at the URL (%s)", esp_err_to_name(err));
        return false;
    }
    if (strncmp(desc.project_name, run->project_name, sizeof(desc.project_name)) != 0) {
        esp_https_ota_abort(h);
        fail("image is for project \"%.32s\", not \"%.32s\"", desc.project_name, run->project_name);
        return false;
    }
    if (strncmp(desc.version, m->version, sizeof(desc.version)) != 0) {
        esp_https_ota_abort(h);
        fail("image has version \"%.32s\", the manifest says \"%s\"", desc.version, m->version);
        return false;
    }
    int total = esp_https_ota_get_image_size(h);
    if (m->size && total > 0 && (uint32_t)total != m->size) {
        esp_https_ota_abort(h);
        fail("image has %d bytes, the manifest says %" PRIu32, total, m->size);
        return false;
    }
    if (total <= 0) {
        total = (int)m->size;
    }

    int last_pct = -1;
    for (;;) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int read = esp_https_ota_get_image_len_read(h);
        int pct = total > 0 ? (int)((int64_t)read * 100 / total) : 0;
        pct = pct > 99 ? 99 : pct;
        if (pct / 5 != last_pct / 5) {
            last_pct = pct;
            portENTER_CRITICAL(&s_mux);
            s_st.progress = (uint8_t)pct;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "download %d %%", pct);
            notify();
        }
        if (deferrable && busy()) {
            esp_https_ota_abort(h);
            ESP_LOGI(TAG, "playback started: download postponed");
            s_auto_pending = true;
            set_state(NET_OTA_AVAILABLE);
            return false;
        }
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(h);
        fail("download of %s failed: %s", m->version, esp_err_to_name(err));
        return false;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        fail("download of %s incomplete", m->version);
        return false;
    }
    const int len = esp_https_ota_get_image_len_read(h);
    if (m->size && (uint32_t)len != m->size) {
        esp_https_ota_abort(h);
        fail("received %d bytes, the manifest says %" PRIu32, len, m->size);
        return false;
    }

    set_state(NET_OTA_VERIFYING);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL || !partition_sha_matches(part, (size_t)len, m->sha256)) {
        esp_https_ota_abort(h);
        fail("SHA-256 of the downloaded image does not match the manifest");
        return false;
    }
    err = esp_https_ota_finish(h);      /* validates the image, sets the boot partition */
    if (err != ESP_OK) {
        fail("image rejected: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "firmware %s written to %s (%d bytes, SHA-256 ok)", m->version, part->label, len);
    s_auto_pending = false;
    set_state(NET_OTA_READY);
    return true;
}

/* ---- jobs ----------------------------------------------------------------------------- */

static void run_check(job_t job)
{
    char url[NET_URL_MAX];
    net_ota_get_url(url, sizeof(url), NULL);
    if (url[0] == '\0') {
        set_state(NET_OTA_DISABLED);
        return;
    }
    set_state(NET_OTA_CHECKING);
    portENTER_CRITICAL(&s_mux);
    s_last_try_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);

    net_manifest_t *m = psram_alloc(sizeof(*m));
    char err[sizeof(s_st.last_error)];
    if (m == NULL) {
        s_last_failed = true;
        fail("out of memory");
        return;
    }
    if (!fetch_manifest(url, m, err, sizeof(err))) {
        s_last_failed = true;
        fail("%s", err);
        free(m);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_last_failed = false;
    s_last_ok_us = esp_timer_get_time();
    strlcpy(s_st.available_version, m->version, sizeof(s_st.available_version));
    strlcpy(s_st.notes, m->notes, sizeof(s_st.notes));
    portEXIT_CRITICAL(&s_mux);

    const char *running = esp_app_get_description()->version;
    switch (net_ota_decide(running, m->version, m->min_version)) {
    case NET_OTA_DECIDE_UP_TO_DATE:
        ESP_LOGI(TAG, "up to date (running %s, offered %s)", running, m->version);
        s_auto_pending = false;
        set_state(NET_OTA_UP_TO_DATE);
        break;
    case NET_OTA_DECIDE_TOO_OLD:
        fail("%s needs at least %s: update over USB first", m->version, m->min_version);
        break;
    case NET_OTA_DECIDE_INVALID:
        fail("invalid version in manifest");
        break;
    case NET_OTA_DECIDE_INSTALL:
        ESP_LOGI(TAG, "update available: %s -> %s", running, m->version);
        if (job == JOB_CHECK || (job == JOB_AUTO && !AUTO_INSTALL)) {
            set_state(NET_OTA_AVAILABLE);
        } else if (job == JOB_AUTO && strcmp(m->version, s_bad_version) == 0) {
            ESP_LOGW(TAG, "%s was rolled back before: not installing it automatically again", m->version);
            set_state(NET_OTA_AVAILABLE);
        } else if (job == JOB_AUTO && busy()) {
            s_auto_pending = true;
            set_state(NET_OTA_AVAILABLE);
        } else if (install(m, job == JOB_AUTO) && (job == JOB_INSTALL || !busy())) {
            do_reboot();
        }
        break;
    }
    free(m);
}

static void worker(void *arg)
{
    job_t job = (job_t)(intptr_t)arg;
    switch (job) {
    case JOB_REBOOT:
        do_reboot();
        break;
    case JOB_ROLLBACK:
        ESP_LOGE(TAG, "firmware %s was not confirmed within %d s: back to the previous firmware",
                 s_st.current_version, CONFIG_MS_OTA_VERIFY_TIMEOUT_S);
        set_state(NET_OTA_REBOOTING);
        vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        break;
    default:
        run_check(job);
        break;
    }
    ESP_LOGI(TAG, "job done, stack headroom %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    portENTER_CRITICAL(&s_mux);
    s_worker = NULL;
    portEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

static esp_err_t spawn(job_t job)
{
    portENTER_CRITICAL(&s_mux);
    bool running = s_worker != NULL;
    if (!running) {
        s_worker = (TaskHandle_t)1;     /* reserved, replaced below */
    }
    portEXIT_CRITICAL(&s_mux);
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }
    TaskHandle_t h = NULL;
    if (xTaskCreatePinnedToCore(worker, "net_ota", WORKER_STACK, (void *)(intptr_t)job, WORKER_PRIO, &h, 0) !=
        pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_worker = NULL;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "no memory for the OTA task");
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_worker == (TaskHandle_t)1) {
        s_worker = h;
    }
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

/* esp_timer task: decide what is due. */
static void on_tick(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    if (!uplink_up()) {
        s_up_since_us = 0;
        return;
    }
    if (s_up_since_us == 0) {
        s_up_since_us = now;
    }
    if (s_worker != NULL) {
        return;
    }
    net_ota_state_t state = get_state();
    if (state == NET_OTA_DISABLED || state == NET_OTA_REBOOTING) {
        return;
    }
    if (busy()) {
        return;     /* nothing while music plays */
    }
    if (state == NET_OTA_READY) {
        spawn(JOB_REBOOT);
        return;
    }
    bool due;
    if (s_auto_pending) {
        due = true;
    } else if (s_last_try_us == 0) {
        due = now - s_up_since_us >= (int64_t)CONFIG_MS_OTA_FIRST_CHECK_S * 1000000;
    } else if (s_last_failed) {
        due = now - s_last_try_us >= RETRY_FAILED_US;
    } else {
        due = now - s_last_try_us >= (int64_t)CONFIG_MS_OTA_CHECK_INTERVAL_H * 3600 * 1000000;
    }
    if (due) {
        spawn(JOB_AUTO);
    }
}

static esp_err_t explicit_job(job_t job)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[NET_URL_MAX];
    net_ota_get_url(url, sizeof(url), NULL);
    net_ota_state_t state = get_state();
    if (url[0] == '\0' || state == NET_OTA_REBOOTING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == NET_OTA_READY) {
        return job == JOB_INSTALL ? spawn(JOB_REBOOT) : ESP_OK;
    }
    return spawn(job);
}

esp_err_t net_ota_check_now(void)
{
    return explicit_job(JOB_CHECK);
}

esp_err_t net_ota_install(void)
{
    return explicit_job(JOB_INSTALL);
}

/* ---- rollback ------------------------------------------------------------------------- */

static void on_verify_timeout(void *arg)
{
    (void)arg;
    if (!s_st.pending_verify) {
        return;
    }
    if (spawn(JOB_ROLLBACK) != ESP_OK) {
        /* a check is running: try again in 10 s */
        esp_timer_start_once(s_verify_timer, 10ULL * 1000000ULL);
    }
}

esp_err_t net_ota_mark_valid(void)
{
    if (!s_st.pending_verify) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "confirming the firmware failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_verify_timer) {
        esp_timer_stop(s_verify_timer);
    }
    portENTER_CRITICAL(&s_mux);
    s_st.pending_verify = false;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "firmware %s confirmed, rollback cancelled", s_st.current_version);
    notify();
    return ESP_OK;
}

static void check_boot_state(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_st.pending_verify = true;
        ESP_LOGW(TAG, "first start of firmware %s after an update: confirm within %d s or it is rolled back",
                 s_st.current_version, CONFIG_MS_OTA_VERIFY_TIMEOUT_S);
        const esp_timer_create_args_t args = {.callback = on_verify_timeout, .name = "net_ota_verify"};
        if (esp_timer_create(&args, &s_verify_timer) == ESP_OK) {
            esp_timer_start_once(s_verify_timer, (uint64_t)CONFIG_MS_OTA_VERIFY_TIMEOUT_S * 1000000ULL);
        }
    }

    if (settings_get_str(SETTINGS_KEY_OTA_BAD_VERSION, s_bad_version, sizeof(s_bad_version)) != ESP_OK) {
        s_bad_version[0] = '\0';
    }
    const esp_partition_t *inv = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (inv && esp_ota_get_partition_description(inv, &d) == ESP_OK) {
        s_st.rolled_back = true;
        strlcpy(s_st.rolled_back_version, d.version, sizeof(s_st.rolled_back_version));
        if (strcmp(s_bad_version, s_st.rolled_back_version) != 0) {
            ESP_LOGW(TAG, "firmware %s did not work and was rolled back", s_st.rolled_back_version);
            strlcpy(s_bad_version, s_st.rolled_back_version, sizeof(s_bad_version));
            settings_set_str(SETTINGS_KEY_OTA_BAD_VERSION, s_bad_version);
        }
    }
}

/* ---- start -------------------------------------------------------------------------------- */

esp_err_t net_ota_start(const char *default_manifest_url)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    strlcpy(s_default_url, default_manifest_url ? default_manifest_url : "", sizeof(s_default_url));
    const char *running = esp_app_get_description()->version;
    strlcpy(s_st.current_version, running, sizeof(s_st.current_version));
    check_boot_state();

    char url[NET_URL_MAX];
    bool from_nvs = false;
    net_ota_get_url(url, sizeof(url), &from_nvs);
    s_st.state = url[0] ? NET_OTA_IDLE : NET_OTA_DISABLED;
    if (url[0] && !net_url_valid(url, false, sizeof(url))) {
        ESP_LOGW(TAG, "manifest URL is not a valid https URL: %s", url);
    }
    ESP_LOGI(TAG, "firmware %s, updates %s%s%s", running, url[0] ? "from " : "disabled (no manifest URL)", url,
             from_nvs ? " (set on the console)" : "");

    const esp_timer_create_args_t args = {.callback = on_tick, .name = "net_ota"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_tick), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick, TICK_US), TAG, "timer start");
    s_started = true;
    return ESP_OK;
}
