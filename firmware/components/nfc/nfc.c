/*
 * nfc - PN532 reader service, see include/nfc.h.
 *
 * Structure:
 *   pn532_frame.c  frame encode/decode, answer parsers (pure C, host-tested)
 *   pn532.c        UART transport and PN532 commands
 *   t2t.c          Type 2 tag read/write procedures on page I/O (pure C, host-tested)
 *   nfc.c          this file: service task, tag presence, events, public API
 *
 * Locking: s.lock (mutex) owns the PN532. The service task takes it for one
 * poll (~30 ms); nfc_write_ref()/nfc_read_now() hold it for the whole
 * wait-for-tag + operation, which pauses polling. s.mux guards s.st and the
 * pending write registration. Callbacks run on the service task without
 * any lock held.
 */
#include "nfc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_pins.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pn532.h"
#include "t2t.h"

static const char *TAG = "nfc";

#define NFC_UART_NUM          UART_NUM_1
#define NFC_BAUD              115200
#define TASK_STACK            4096
#define TASK_PRIO             4
#define TASK_CORE             0
#define MEM_CAP               1024   /* data-area buffer: NTAG216 has 888 bytes    */
#define REMOVE_MISSES         3      /* polls without the tag before TAG_REMOVED       */
#define CONFIRM_POLL_MS       100    /* poll interval while a lying tag is missing:
                                        removal is reported after ~0.5-1 s            */
#define READ_ATTEMPTS         5      /* failed reads of a new tag before giving up  */
#define LOST_AFTER_FAILS      2      /* polls with reader errors before re-init     */
#define BACKOFF_MIN_MS        1000
#define BACKOFF_MAX_MS        30000
#define FIELD_SETTLE_MS       5      /* tag power-up after RF field on              */
#define WAIT_TAG_STEP_MS      100    /* write/read: tag search period               */
#define LOCK_STEP_MS          200
#define POLL_MIN_MS           50
#define POLL_MAX_MS           5000
#define POLL_DEFAULT_MS       300

typedef struct {
    uint8_t mem[MEM_CAP];
    char    link[NFC_TAG_LINK_MAX];
} work_t;

/* Current tag as seen by the poll loop (service task only). */
typedef struct {
    bool    present;      /* seen recently                                   */
    bool    reported;     /* TAG_PRESENT / TAG_WRITTEN emitted               */
    uint8_t uid[PN532_UID_MAX];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t misses;
    uint8_t read_tries;
    bool    field_on;     /* RF field left on: the tag stays powered and selected */
} cur_tag_t;

#define MAX_EVENTS 6   /* worst case per loop: 2 (write) + 2 (poll) + 1 (reader lost) */

static struct {
    SemaphoreHandle_t  lock;          /* created once, never deleted           */
    SemaphoreHandle_t  done;          /* given by the task when it exits       */
    portMUX_TYPE       mux;
    nfc_status_t       st;            /* guarded by mux                        */
    TaskHandle_t       task;
    volatile bool      stop;
    volatile uint32_t  interval_ms;
    nfc_event_cb_t     cb;
    void              *ctx;
    work_t            *work;          /* PSRAM; used under lock                */
    bool               reader_ready;  /* written by the task under lock        */
    /* registration of a written tag, applied by the task (guarded by mux) */
    bool               w_pending;
    pn532_target_a_t   w_tg;
    uint16_t           w_model;
    spotify_ref_t      w_ref;
} s = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
    .interval_ms = POLL_DEFAULT_MS,
};

/* ---- helpers --------------------------------------------------------------------- */

static void delay_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void uid_str(const uint8_t *uid, size_t len, char *out, size_t cap)
{
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len && pos + 3 < cap; i++) {
        pos += (size_t)snprintf(out + pos, cap - pos, "%s%02X", i ? ":" : "", uid[i]);
    }
}

#define ST_LOCKED(...)               \
    do {                             \
        portENTER_CRITICAL(&s.mux);  \
        __VA_ARGS__;                 \
        portEXIT_CRITICAL(&s.mux);   \
    } while (0)

static void set_text(char *dst, const char *src)
{
    char tmp[NFC_ERR_TEXT_MAX];
    snprintf(tmp, sizeof tmp, "%s", src ? src : "");
    ST_LOCKED(memcpy(dst, tmp, sizeof tmp));
}

static void count_comm_error(void)
{
    set_text(s.st.last_error, pn532_last_error());
    ST_LOCKED(s.st.comm_errors++);
}

static bool take_lock(uint32_t ms)
{
    return xSemaphoreTake(s.lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void give_lock(void)
{
    xSemaphoreGive(s.lock);
}

static void emit(const nfc_event_t *ev)
{
    if (s.cb) {
        s.cb(ev, s.ctx);
    }
}

static void fill_uid(nfc_event_t *ev, const uint8_t *uid, uint8_t len)
{
    ev->uid_len = len < sizeof ev->uid ? len : sizeof ev->uid;
    memcpy(ev->uid, uid, ev->uid_len);
}

const char *nfc_event_name(nfc_event_id_t id)
{
    switch (id) {
    case NFC_EVT_TAG_PRESENT:  return "TAG_PRESENT";
    case NFC_EVT_TAG_REMOVED:  return "TAG_REMOVED";
    case NFC_EVT_READER_ERROR: return "READER_ERROR";
    case NFC_EVT_READER_OK:    return "READER_OK";
    case NFC_EVT_TAG_WRITTEN:  return "TAG_WRITTEN";
    }
    return "?";
}

/* ---- tag I/O for t2t.c (called under lock) ------------------------------------------ */

static t2t_io_status_t io_class(esp_err_t e)
{
    return e == ESP_OK ? T2T_IO_OK : e == ESP_FAIL ? T2T_IO_TAG : T2T_IO_READER;
}

static t2t_io_status_t io_read16(void *ctx, uint16_t page, uint8_t out[16])
{
    (void)ctx;
    const uint8_t cmd[2] = { 0x30, (uint8_t)page };   /* NTAG READ */
    size_t n = 0;
    esp_err_t e = pn532_data_exchange(cmd, sizeof cmd, out, 16, &n);
    if (e == ESP_OK && n != 16) {
        return T2T_IO_TAG;
    }
    return io_class(e);
}

static t2t_io_status_t io_write4(void *ctx, uint16_t page, const uint8_t data[4])
{
    (void)ctx;
    const uint8_t cmd[6] = { 0xA2, (uint8_t)page, data[0], data[1], data[2], data[3] };   /* NTAG WRITE */
    uint8_t rx[16];
    size_t n = 0;
    return io_class(pn532_data_exchange(cmd, sizeof cmd, rx, sizeof rx, &n));
}

static t2t_io_status_t io_get_version(void *ctx, uint8_t out[8])
{
    (void)ctx;
    /* GET_VERSION is no MIFARE command the PN532 knows for InDataExchange: send
     * it raw (CRC is still added and checked by the PN532). */
    static const uint8_t cmd[1] = { 0x60 };
    uint8_t rx[16];
    size_t n = 0;
    esp_err_t e = pn532_communicate_thru(cmd, sizeof cmd, rx, sizeof rx, &n);
    if (e == ESP_OK && n != 8) {
        return T2T_IO_TAG;
    }
    if (e == ESP_OK) {
        memcpy(out, rx, 8);
    }
    return io_class(e);
}

static const t2t_io_t s_io = {
    .read16 = io_read16,
    .write4 = io_write4,
    .get_version = io_get_version,
    .ctx = NULL,
};

/* ---- reader operations (under lock) ------------------------------------------------ */

static esp_err_t select_tag(pn532_target_a_t *tg, bool *found)
{
    *found = false;
    esp_err_t e = pn532_rf_field(true);
    if (e != ESP_OK) {
        return e;
    }
    delay_ms(FIELD_SETTLE_MS);
    return pn532_list_106a(tg, found);
}

static void field_off(void)
{
    if (pn532_rf_field(false) != ESP_OK) {
        count_comm_error();
    }
}

/* Model via GET_VERSION; 0 if the tag does not answer it (call last: a NAK
 * puts the tag back to IDLE). */
static uint16_t query_model(void)
{
    uint8_t ver[8];
    nfc_tag_ntag_info_t info;
    if (io_get_version(NULL, ver) == T2T_IO_OK && nfc_tag_ntag_from_version(ver, &info) == NFC_TAG_OK) {
        return info.model;
    }
    return 0;
}

typedef struct {
    bool          read_ok;
    bool          has_ref;
    spotify_ref_t ref;
    nfc_tag_err_t tag_err;
    t2t_io_status_t io;      /* != OK: reading failed on the link              */
    uint16_t      model;
    size_t        bytes;
    char          error[NFC_ERR_TEXT_MAX];
} tag_read_t;

static void read_selected(const pn532_target_a_t *tg, tag_read_t *r)
{
    memset(r, 0, sizeof *r);
    if (tg->sel_res != 0x00) {
        snprintf(r->error, sizeof r->error, "not an NFC Forum Type 2 tag (SAK 0x%02X)", tg->sel_res);
        return;
    }
    t2t_outcome_t o;
    t2t_result_t res = t2t_read_ref(&s_io, s.work->mem, MEM_CAP, &r->ref, s.work->link, sizeof s.work->link, &o);
    r->bytes = o.bytes_read;
    switch (res) {
    case T2T_OK:
        r->read_ok = r->has_ref = true;
        break;
    case T2T_ERR_TAG:
        r->read_ok = true;
        r->tag_err = o.tag_err;
        snprintf(r->error, sizeof r->error, "%s", nfc_tag_err_str(o.tag_err));
        break;
    default:
        r->io = o.io;
        snprintf(r->error, sizeof r->error, "read failed: %s", pn532_last_error());
        return;
    }
    r->model = query_model();
}

static void log_tag(const char *what, const pn532_target_a_t *tg, const tag_read_t *r)
{
    char u[32], kind[24], uri[SPOTIFY_REF_URI_MAX];
    uid_str(tg->uid, tg->uid_len, u, sizeof u);
    if (r->model) {
        snprintf(kind, sizeof kind, "NTAG%u", (unsigned)r->model);
    } else {
        snprintf(kind, sizeof kind, "SAK %02X", tg->sel_res);
    }
    if (r->has_ref) {
        spotify_ref_to_uri(&r->ref, uri, sizeof uri);
        ESP_LOGI(TAG, "%s %s (%s): %s", what, u, kind, uri);
    } else {
        const char *link = r->read_ok ? s.work->link : "";
        ESP_LOGI(TAG, "%s %s (%s): no Spotify reference - %s%s%.80s%s", what, u, kind, r->error,
                 link[0] ? " (\"" : "", link, link[0] ? "\")" : "");
    }
}

/* ---- poll loop ------------------------------------------------------------------------ */

typedef struct {
    nfc_event_t ev[MAX_EVENTS];
    int         n;
} ev_list_t;

static nfc_event_t *ev_add(ev_list_t *l, nfc_event_id_t id)
{
    nfc_event_t *ev = &l->ev[l->n < MAX_EVENTS ? l->n++ : MAX_EVENTS - 1];
    memset(ev, 0, sizeof *ev);
    ev->id = id;
    return ev;
}

static void tag_gone(cur_tag_t *cur, ev_list_t *l)
{
    if (cur->present && cur->reported) {
        nfc_event_t *ev = ev_add(l, NFC_EVT_TAG_REMOVED);
        fill_uid(ev, cur->uid, cur->uid_len);
        char u[32];
        uid_str(cur->uid, cur->uid_len, u, sizeof u);
        ESP_LOGI(TAG, "tag %s removed", u);
    }
    memset(cur, 0, sizeof *cur);
    ST_LOCKED({
        s.st.tag_present = false;
        s.st.has_ref = false;
        s.st.uid_len = 0;
        s.st.model = 0;
        s.st.sak = 0;
        s.st.tag_error[0] = '\0';
    });
}

static void set_current(cur_tag_t *cur, const pn532_target_a_t *tg)
{
    memset(cur, 0, sizeof *cur);
    cur->present = true;
    cur->sak = tg->sel_res;
    cur->uid_len = tg->uid_len;
    memcpy(cur->uid, tg->uid, tg->uid_len);
}

static void publish_tag(const pn532_target_a_t *tg, uint16_t model, bool has_ref, const spotify_ref_t *ref,
                        const char *err)
{
    ST_LOCKED({
        s.st.tag_present = true;
        s.st.uid_len = tg->uid_len;
        memcpy(s.st.uid, tg->uid, tg->uid_len);
        s.st.sak = tg->sel_res;
        s.st.model = model;
        s.st.has_ref = has_ref;
        if (has_ref) {
            s.st.ref = *ref;
        }
    });
    set_text(s.st.tag_error, err);
}

/* Same tag as the current one? Phones and other card emulators present a new
 * random UID (4 bytes starting with 08, ISO/IEC 14443-3) on every field
 * activation; as the field is switched off between polls, they would look like
 * a new tag each time. Two random-UID targets with the same SAK count as one. */
static bool same_tag(const cur_tag_t *cur, const pn532_target_a_t *tg)
{
    if (!cur->present || cur->uid_len != tg->uid_len) {
        return false;
    }
    if (tg->uid_len == 4 && tg->uid[0] == 0x08 && cur->uid[0] == 0x08) {
        return cur->sak == tg->sel_res;
    }
    return memcmp(cur->uid, tg->uid, tg->uid_len) == 0;
}

/* A tag written by nfc_write_ref(): becomes the current tag without TAG_PRESENT. */
static void apply_written(cur_tag_t *cur, ev_list_t *l)
{
    bool pending;
    pn532_target_a_t tg;
    uint16_t model;
    spotify_ref_t ref;
    portENTER_CRITICAL(&s.mux);
    pending = s.w_pending;
    tg = s.w_tg;
    model = s.w_model;
    ref = s.w_ref;
    s.w_pending = false;
    portEXIT_CRITICAL(&s.mux);
    if (!pending) {
        return;
    }
    if (cur->present && !same_tag(cur, &tg)) {
        tag_gone(cur, l);
    }
    set_current(cur, &tg);
    cur->reported = true;
    publish_tag(&tg, model, true, &ref, "");
    nfc_event_t *ev = ev_add(l, NFC_EVT_TAG_WRITTEN);
    fill_uid(ev, tg.uid, tg.uid_len);
    ev->has_ref = true;
    ev->ref = ref;
    ev->read_ok = true;
}

/* One poll. Returns ESP_OK or a reader communication error. */
static esp_err_t poll_once(cur_tag_t *cur, ev_list_t *l)
{
    /* A reported tag is kept powered (field on) and only asked for its first
     * page: a weakly coupled tag stays alive much more reliably than with a
     * power-up and anticollision on every poll, which made tags lying on the
     * reader "disappear" for a few seconds (found on hardware). */
    if (cur->present && cur->reported && cur->field_on) {
        uint8_t page[16];
        t2t_io_status_t io = io_read16(NULL, 0, page);
        ST_LOCKED(s.st.polls++);
        if (io == T2T_IO_OK && (cur->uid_len < 3 || memcmp(page, cur->uid, 3) == 0)) {
            cur->misses = 0;
            return ESP_OK;
        }
        cur->field_on = false;
        if (io == T2T_IO_READER) {
            count_comm_error();
            pn532_rf_field(false);
            return ESP_ERR_INVALID_RESPONSE;
        }
        /* no answer: power-cycle the tag and look for it again */
        field_off();
        delay_ms(10);
    }

    pn532_target_a_t tg;
    bool found = false;
    esp_err_t e = select_tag(&tg, &found);
    if (e != ESP_OK) {
        count_comm_error();
        pn532_rf_field(false);
        return e;
    }
    ST_LOCKED(s.st.polls++);

    if (!found) {
        field_off();
        if (cur->present && ++cur->misses >= REMOVE_MISSES) {
            tag_gone(cur, l);
        }
        return ESP_OK;
    }

    if (!same_tag(cur, &tg)) {
        tag_gone(cur, l);          /* a different tag replaced it (no-op if none) */
        set_current(cur, &tg);
    }
    cur->misses = 0;
    if (cur->reported) {
        cur->field_on = true;      /* still there: keep it powered (see above) */
        return ESP_OK;
    }

    tag_read_t r;
    read_selected(&tg, &r);
    field_off();
    if (r.io == T2T_IO_READER) {
        count_comm_error();
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (r.io == T2T_IO_TAG) {
        ST_LOCKED(s.st.read_errors++);
        if (++cur->read_tries < READ_ATTEMPTS) {
            return ESP_OK;         /* retry on the next poll */
        }
    }
    cur->reported = true;
    log_tag("tag", &tg, &r);
    publish_tag(&tg, r.model, r.has_ref, &r.ref, r.error);
    ST_LOCKED(s.st.tags_detected++);
    nfc_event_t *ev = ev_add(l, NFC_EVT_TAG_PRESENT);
    fill_uid(ev, tg.uid, tg.uid_len);
    ev->has_ref = r.has_ref;
    ev->ref = r.ref;
    ev->read_ok = r.read_ok;
    ev->tag_err = r.read_ok ? r.tag_err : NFC_TAG_OK;
    return ESP_OK;
}

/* Waits ms or until nfc_stop()/a write registration wakes the task. */
static void task_wait(uint32_t ms)
{
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

static void emit_all(const ev_list_t *l)
{
    for (int i = 0; i < l->n; i++) {
        emit(&l->ev[i]);
    }
}

static void nfc_task(void *arg)
{
    (void)arg;
    cur_tag_t cur;
    memset(&cur, 0, sizeof cur);
    uint32_t backoff = BACKOFF_MIN_MS;
    bool error_reported = false;
    bool quiet = false;            /* longest back-off step already logged */
    int fails = 0;

    while (!s.stop) {
        if (!s.reader_ready) {
            if (!take_lock(LOCK_STEP_MS)) {
                continue;
            }
            pn532_fw_t fw;
            esp_err_t e = pn532_init_reader(&fw);
            if (e == ESP_OK) {
                s.reader_ready = true;
            }
            give_lock();
            if (e == ESP_OK) {
                char f[24];
                snprintf(f, sizeof f, "PN532 v%u.%u", fw.ver, fw.rev);
                ST_LOCKED({
                    memcpy(s.st.firmware, f, sizeof f);
                    s.st.reader_ok = true;
                    s.st.reader_inits++;
                });
                ESP_LOGI(TAG, "%s%s ready (support 0x%02X), polling every %" PRIu32 " ms", f,
                         error_reported ? " answering again," : "", fw.support, s.interval_ms);
                error_reported = false;
                quiet = false;
                backoff = BACKOFF_MIN_MS;
                fails = 0;
                nfc_event_t ev = { .id = NFC_EVT_READER_OK };
                emit(&ev);
                continue;
            }
            set_text(s.st.last_error, pn532_last_error());
            ST_LOCKED(s.st.reader_ok = false);
            /* One line per back-off step; at the longest step only once, so a
             * board without a reader does not fill the console. */
            if (!quiet) {
                ESP_LOGW(TAG, "PN532 not answering (%s) - check wiring, DIP switches (HSU), power; retry in %" PRIu32
                         " s%s", pn532_last_error(), backoff / 1000,
                         backoff == BACKOFF_MAX_MS ? " (further retries every 30 s are not logged, see \"nfc status\")" : "");
                quiet = backoff == BACKOFF_MAX_MS;
            } else {
                ESP_LOGD(TAG, "PN532 still not answering (%s)", pn532_last_error());
            }
            if (!error_reported) {
                error_reported = true;
                nfc_event_t ev = { .id = NFC_EVT_READER_ERROR };
                emit(&ev);
            }
            task_wait(backoff);
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
            continue;
        }

        if (!take_lock(LOCK_STEP_MS)) {
            continue;              /* a write/read holds the reader: poll later */
        }
        ev_list_t l = { .n = 0 };
        apply_written(&cur, &l);
        esp_err_t e = poll_once(&cur, &l);
        if (e != ESP_OK && ++fails >= LOST_AFTER_FAILS) {
            s.reader_ready = false;
        }
        give_lock();
        if (e == ESP_OK) {
            fails = 0;
        }
        if (!s.reader_ready) {
            tag_gone(&cur, &l);
            ST_LOCKED(s.st.reader_ok = false);
            ESP_LOGW(TAG, "PN532 stopped answering (%s), re-initialising", pn532_last_error());
            error_reported = true;
            quiet = false;
            ev_add(&l, NFC_EVT_READER_ERROR);
            backoff = BACKOFF_MIN_MS;
            fails = 0;
        }
        emit_all(&l);
        /* a lying tag stopped answering: confirm quickly, so music pauses
         * soon after the tag is taken away */
        uint32_t wait = !s.reader_ready ? BACKOFF_MIN_MS
                        : (cur.present && cur.misses > 0) ? CONFIRM_POLL_MS : s.interval_ms;
        task_wait(wait);
    }

    if (take_lock(1000)) {
        if (s.reader_ready) {
            pn532_rf_field(false);
        }
        s.reader_ready = false;
        give_lock();
    }
    xSemaphoreGive(s.done);
    vTaskDelete(NULL);
}

/* ---- public API -------------------------------------------------------------------------- */

esp_err_t nfc_start(void (*cb)(const nfc_event_t *, void *), void *ctx)
{
    if (s.task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.done = xSemaphoreCreateBinary();
        if (!s.lock || !s.done) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s.work) {
        s.work = heap_caps_malloc(sizeof(work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.work) {
            s.work = malloc(sizeof(work_t));
        }
        if (!s.work) {
            return ESP_ERR_NO_MEM;
        }
    }
    const pn532_cfg_t cfg = {
        .uart_num = NFC_UART_NUM,
        .tx_gpio = BOARD_NFC_UART_TX_GPIO,
        .rx_gpio = BOARD_NFC_UART_RX_GPIO,
        .rstpd_n_gpio = BOARD_NFC_RSTPD_N_GPIO,
        .baud = NFC_BAUD,
    };
    esp_err_t err = pn532_open(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d (TX %d, RX %d): %s", NFC_UART_NUM, cfg.tx_gpio, cfg.rx_gpio, esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s.mux);
    memset(&s.st, 0, sizeof s.st);
    s.st.running = true;
    s.st.poll_interval_ms = s.interval_ms;
    s.w_pending = false;
    portEXIT_CRITICAL(&s.mux);
    s.cb = cb;
    s.ctx = ctx;
    s.stop = false;
    s.reader_ready = false;
    xSemaphoreTake(s.done, 0);
    if (xTaskCreatePinnedToCore(nfc_task, "nfc", TASK_STACK, NULL, TASK_PRIO, &s.task, TASK_CORE) != pdPASS) {
        s.task = NULL;
        pn532_close();
        ST_LOCKED(s.st.running = false);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started: UART%d TX GPIO%d, RX GPIO%d, RSTPD_N GPIO%d, %d baud", NFC_UART_NUM, cfg.tx_gpio,
             cfg.rx_gpio, cfg.rstpd_n_gpio, NFC_BAUD);
    return ESP_OK;
}

void nfc_stop(void)
{
    if (!s.task || xTaskGetCurrentTaskHandle() == s.task) {
        return;
    }
    s.stop = true;
    xTaskNotifyGive(s.task);
    xSemaphoreTake(s.done, portMAX_DELAY);
    s.task = NULL;
    xSemaphoreTake(s.lock, portMAX_DELAY);   /* a write in progress finishes first */
    pn532_close();
    xSemaphoreGive(s.lock);
    ST_LOCKED({
        s.st.running = false;
        s.st.reader_ok = false;
        s.st.tag_present = false;
    });
    ESP_LOGI(TAG, "stopped");
}

void nfc_set_poll_interval_ms(uint32_t ms)
{
    ms = ms < POLL_MIN_MS ? POLL_MIN_MS : ms > POLL_MAX_MS ? POLL_MAX_MS : ms;
    s.interval_ms = ms;
    ST_LOCKED(s.st.poll_interval_ms = ms);
}

void nfc_get_status(nfc_status_t *out)
{
    if (!out) {
        return;
    }
    ST_LOCKED(*out = s.st);
}

/* Takes the reader and waits for a tag. On ESP_OK the lock is held and the
 * tag is selected (RF field on). */
static esp_err_t acquire_tag(uint32_t timeout_ms, pn532_target_a_t *tg, char *err, size_t err_cap)
{
    if (!s.task || s.stop) {
        snprintf(err, err_cap, "nfc service not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s.task) {
        snprintf(err, err_cap, "not allowed from the nfc callback");
        return ESP_ERR_INVALID_STATE;
    }
    int64_t deadline = now_ms() + timeout_ms;
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(timeout_ms > 1000 ? timeout_ms : 1000)) != pdTRUE) {
        snprintf(err, err_cap, "reader busy");
        return ESP_ERR_TIMEOUT;
    }
    if (!s.reader_ready) {
        give_lock();
        snprintf(err, err_cap, "reader not ready (%s)", pn532_last_error());
        return ESP_ERR_INVALID_STATE;
    }
    /* the poll loop may keep a tag powered and selected: start from a reset
     * tag, otherwise it would not answer the anticollision */
    pn532_rf_field(false);
    delay_ms(10);
    for (;;) {
        bool found = false;
        esp_err_t e = select_tag(tg, &found);
        if (e != ESP_OK) {
            count_comm_error();
            pn532_rf_field(false);
            give_lock();
            snprintf(err, err_cap, "reader: %s", pn532_last_error());
            return ESP_FAIL;
        }
        if (found) {
            return ESP_OK;
        }
        pn532_rf_field(false);
        if (now_ms() >= deadline || s.stop) {
            give_lock();
            snprintf(err, err_cap, "no tag within %" PRIu32 " s", (timeout_ms + 999) / 1000);
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(WAIT_TAG_STEP_MS);
    }
}

static esp_err_t write_failed(esp_err_t code, const char *text)
{
    set_text(s.st.last_write_error, text);
    ST_LOCKED(s.st.writes_failed++);
    ESP_LOGW(TAG, "write failed: %s", text);
    return code;
}

esp_err_t nfc_write_ref(const spotify_ref_t *ref, uint32_t timeout_ms)
{
    char err[2 * NFC_ERR_TEXT_MAX];   /* write_failed() keeps NFC_ERR_TEXT_MAX - 1 */
    if (!spotify_ref_is_valid(ref)) {
        return write_failed(ESP_ERR_INVALID_ARG, "invalid Spotify reference");
    }
    pn532_target_a_t tg;
    esp_err_t e = acquire_tag(timeout_ms, &tg, err, sizeof err);
    if (e != ESP_OK) {
        return write_failed(e, err);
    }
    if (tg.sel_res != 0x00) {
        pn532_rf_field(false);
        give_lock();
        snprintf(err, sizeof err, "not an NTAG213/215/216 (SAK 0x%02X)", tg.sel_res);
        return write_failed(ESP_ERR_NOT_SUPPORTED, err);
    }

    t2t_outcome_t o;
    t2t_result_t res = t2t_write_ref(&s_io, ref, &o);
    if (res == T2T_OK) {
        portENTER_CRITICAL(&s.mux);    /* registered before the poll loop runs again */
        s.w_pending = true;
        s.w_tg = tg;
        s.w_model = o.model;
        s.w_ref = *ref;
        portEXIT_CRITICAL(&s.mux);
    }
    char pn_err[NFC_ERR_TEXT_MAX];
    snprintf(pn_err, sizeof pn_err, "%s", pn532_last_error());
    field_off();
    give_lock();

    switch (res) {
    case T2T_OK: {
        char u[32], uri[SPOTIFY_REF_URI_MAX];
        uid_str(tg.uid, tg.uid_len, u, sizeof u);
        spotify_ref_to_uri(ref, uri, sizeof uri);
        ESP_LOGI(TAG, "wrote %s to NTAG%u %s (%u page writes%s), verified", uri, (unsigned)o.model, u, o.writes,
                 o.cc_written ? ", CC formatted" : "");
        set_text(s.st.last_write_error, "");
        ST_LOCKED(s.st.writes_ok++);
        TaskHandle_t task = s.task;
        if (task) {
            xTaskNotifyGive(task);
        }
        return ESP_OK;
    }
    case T2T_ERR_TAG:
        snprintf(err, sizeof err, "%s", nfc_tag_err_str(o.tag_err));
        switch (o.tag_err) {
        case NFC_TAG_ERR_READ_ONLY:
        case NFC_TAG_ERR_PASSWORD:
            return write_failed(ESP_ERR_NOT_ALLOWED, err);
        case NFC_TAG_ERR_NO_SPACE:
            return write_failed(ESP_ERR_INVALID_SIZE, err);
        case NFC_TAG_ERR_UNSUPPORTED:
            if (o.model == 0) {
                return write_failed(ESP_ERR_NOT_SUPPORTED, "not an NTAG213/215/216");
            }
            return write_failed(ESP_ERR_NOT_SUPPORTED, err);
        default:
            return write_failed(ESP_ERR_NOT_SUPPORTED, err);
        }
    case T2T_ERR_VERIFY:
        return write_failed(ESP_ERR_INVALID_CRC, "read back differs from the written data");
    case T2T_ERR_IO:
    default:
        if (o.io == T2T_IO_READER) {
            count_comm_error();
        }
        if (o.reads == 0 && o.writes == 0 && o.io == T2T_IO_TAG) {
            snprintf(err, sizeof err, "no GET_VERSION answer: not an NTAG21x or tag moved");
            return write_failed(ESP_ERR_NOT_SUPPORTED, err);
        }
        snprintf(err, sizeof err, "%s%.40s", o.writes ? "aborted mid-write (tag now empty): " : "",
                 o.io == T2T_IO_TAG ? "tag moved away" : pn_err);
        return write_failed(ESP_FAIL, err);
    }
}

esp_err_t nfc_write_url(const char *link, uint32_t timeout_ms)
{
    spotify_ref_t ref;
    nfc_tag_err_t e = nfc_tag_parse_link_ex(link ? link : "", &ref);
    if (e != NFC_TAG_OK) {
        return write_failed(ESP_ERR_INVALID_ARG, nfc_tag_err_str(e));
    }
    return nfc_write_ref(&ref, timeout_ms);
}

esp_err_t nfc_read_now(nfc_read_result_t *out, uint32_t timeout_ms)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    pn532_target_a_t tg;
    esp_err_t e = acquire_tag(timeout_ms, &tg, out->error, sizeof out->error);
    if (e != ESP_OK) {
        return e;
    }
    tag_read_t r;
    read_selected(&tg, &r);
    snprintf(out->link, sizeof out->link, "%s", r.read_ok ? s.work->link : "");
    field_off();
    give_lock();

    out->uid_len = tg.uid_len;
    memcpy(out->uid, tg.uid, tg.uid_len);
    out->sak = tg.sel_res;
    out->model = r.model;
    out->bytes_read = r.bytes;
    out->has_ref = r.has_ref;
    out->ref = r.ref;
    out->tag_err = r.read_ok ? r.tag_err : NFC_TAG_OK;
    snprintf(out->error, sizeof out->error, "%s", r.error);
    if (r.io == T2T_IO_READER) {
        count_comm_error();
    }
    return r.io == T2T_IO_OK ? ESP_OK : ESP_FAIL;
}
