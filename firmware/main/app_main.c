/*
 * Music Streamer - application entry point (bench firmware).
 *
 * Boot sequence:
 *   settings (NVS) -> netif/event loop -> audio_out (I2S + DAC) -> serial
 *   console -> status LED -> WiFi (stored networks, non-blocking; setup
 *   hotspot when none works) -> OTA scheduler -> supervisor task: SNTP +
 *   Spotify Connect once the network is up and the Spotify credentials are
 *   set, confirms an updated firmware -> periodic heap statistics.
 *
 * Nothing secret is compiled in: WiFi networks and the Spotify developer
 * credentials are entered on the serial console (see console.c) and stored in
 * NVS. The real application state machine (work package P7) replaces this
 * file; display, NFC and Web API are not part of the bench build.
 */
#include <inttypes.h>
#include "esp_system.h"
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_settings.h"
#include "app_ui.h"
#include "audio_out.h"
#include "bench_util.h"
#include "board_pins.h"
#include "console.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_ota.h"
#include "net_portal.h"
#include "net_qemu.h"
#include "net_time.h"
#include "net_wifi.h"
#include "nfc.h"
#include "nfc_tag.h"
#include "sdkconfig.h"
#include "settings.h"
#include "spotify_connect.h"

static const char *TAG = "app";

/* ---- shared state ---------------------------------------------------------- */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static app_spotify_info_t s_sp;              /* guarded by s_mux */
static volatile bool s_playing;
static volatile bool s_qemu_net_up;
static TaskHandle_t s_supervisor;
static volatile int32_t s_volume_to_save = -1;   /* -1: nothing to store */

void app_get_spotify_info(app_spotify_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_sp;
    portEXIT_CRITICAL(&s_mux);
}

static void set_spotify_state(app_spotify_state_t state)
{
    portENTER_CRITICAL(&s_mux);
    s_sp.state = state;
    portEXIT_CRITICAL(&s_mux);
}

const char *app_spotify_state_name(app_spotify_state_t state)
{
    switch (state) {
    case APP_SPOTIFY_NOT_CONFIGURED:  return "not configured (use: spotify creds <id> <secret>)";
    case APP_SPOTIFY_WAITING_NETWORK: return "waiting for network";
    case APP_SPOTIFY_START_FAILED:    return "start failed (see log)";
    case APP_SPOTIFY_CONNECTING:      return "connecting";
    case APP_SPOTIFY_WAITING_LOGIN:   return "waiting: pick the device once in the Spotify app (same WiFi)";
    case APP_SPOTIFY_LOGGED_IN:       return "logged in";
    }
    return "?";
}

bool app_spotify_playing(void)
{
    return s_playing;
}

bool app_network_up(void)
{
#if CONFIG_MS_NET_QEMU_OPENETH
    return s_qemu_net_up;
#else
    return net_wifi_is_connected();
#endif
}

uint32_t app_network_ip(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0;
}

void app_spotify_settings_changed(void)
{
    if (s_supervisor) {
        xTaskNotifyGive(s_supervisor);
    }
}

/* ---- heap statistics ------------------------------------------------------- */

/* Free / lowest-ever free heap, separately for internal RAM and PSRAM. Internal
 * RAM is the scarce resource (WiFi, TLS, DMA, later LVGL draw buffers). */
static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap [%s] internal: free %u, min %u, largest %u | psram: free %u, min %u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- Spotify ----------------------------------------------------------------- */

static void on_spotify_event_log(const spotify_event_t *ev);

/* Runs on spotify_connect's event task. Keep it short. */
static void on_spotify_event(const spotify_event_t *ev, void *ctx)
{
    (void)ctx;
    on_spotify_event_log(ev);
    app_ui_spotify_event(ev);   /* after the shared state above is updated */
}

/* State + log part of on_spotify_event(). */
static void on_spotify_event_log(const spotify_event_t *ev)
{
    switch (ev->id) {
    case SPOTIFY_EVENT_ZEROCONF_WAITING:
        set_spotify_state(APP_SPOTIFY_WAITING_LOGIN);
        ESP_LOGI(TAG, "spotify: open the Spotify app on a phone in the same WiFi and pick this device");
        break;
    case SPOTIFY_EVENT_LOGGED_IN:
        portENTER_CRITICAL(&s_mux);
        s_sp.state = APP_SPOTIFY_LOGGED_IN;
        s_sp.stored_credentials = ev->data.session.stored_credentials;
        snprintf(s_sp.user, sizeof(s_sp.user), "%s", ev->data.session.username);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "spotify: logged in as %s (%s)", ev->data.session.username,
                 ev->data.session.stored_credentials ? "stored credentials" : "zeroconf");
        break;
    case SPOTIFY_EVENT_LOGIN_FAILED:
        set_spotify_state(APP_SPOTIFY_WAITING_LOGIN);
        ESP_LOGW(TAG, "spotify: login failed, waiting for a new login from the app");
        break;
    case SPOTIFY_EVENT_PLAYBACK_STARTED:
    case SPOTIFY_EVENT_PLAYBACK_RESUMED:
        s_playing = !ev->data.playback.paused;
        ESP_LOGI(TAG, "spotify: %s at %" PRIu32 " ms",
                 s_playing ? "playing" : "loaded (paused)", ev->data.playback.position_ms);
        break;
    case SPOTIFY_EVENT_PLAYBACK_PAUSED:
        s_playing = false;
        ESP_LOGI(TAG, "spotify: paused at %" PRIu32 " ms", ev->data.playback.position_ms);
        break;
    case SPOTIFY_EVENT_SEEK:
        ESP_LOGI(TAG, "spotify: seek to %" PRIu32 " ms", ev->data.playback.position_ms);
        break;
    case SPOTIFY_EVENT_TRACK_CHANGED:
        portENTER_CRITICAL(&s_mux);
        snprintf(s_sp.track, sizeof(s_sp.track), "%s - %s", ev->data.track.artist, ev->data.track.title);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "spotify: track \"%s\" / \"%s\" / \"%s\", %" PRIu32 " ms",
                 ev->data.track.title, ev->data.track.artist, ev->data.track.album,
                 ev->data.track.duration_ms);
        break;
    case SPOTIFY_EVENT_VOLUME:
        ESP_LOGI(TAG, "spotify: volume %u/65535", ev->data.volume);
        s_volume_to_save = ev->data.volume;   /* the supervisor stores it (NVS) */
        break;
    case SPOTIFY_EVENT_PLAYBACK_STOPPED:
        s_playing = false;
        ESP_LOGI(TAG, "spotify: end of queue");
        break;
    case SPOTIFY_EVENT_INACTIVE:
        s_playing = false;
        ESP_LOGI(TAG, "spotify: device no longer active");
        break;
    case SPOTIFY_EVENT_PLAY_URI_FAILED:
        ESP_LOGW(TAG, "spotify: could not start the requested music");
        break;
    }
}

static void try_start_spotify(void)
{
    char client_id[APP_CREDENTIAL_MAX];
    char client_secret[APP_CREDENTIAL_MAX];
    char name[APP_DEVICE_NAME_MAX];
    if (app_settings_spotify_credentials(client_id, sizeof(client_id), client_secret,
                                         sizeof(client_secret)) == APP_SETTING_UNSET) {
        set_spotify_state(APP_SPOTIFY_NOT_CONFIGURED);
        return;
    }
    if (!app_network_up()) {
        set_spotify_state(APP_SPOTIFY_WAITING_NETWORK);
        return;
    }
    app_settings_device_name(name, sizeof(name));
    const spotify_connect_config_t cfg = {
        .device_name = name,
        .client_id = client_id,
        .client_secret = client_secret,
        .bitrate = (spotify_bitrate_t)app_settings_spotify_bitrate(),
        .zeroconf_port = 0,
        .initial_volume = settings_get_u32_or(SETTINGS_KEY_VOLUME, UINT16_MAX),
        .event_cb = on_spotify_event,
        .user_ctx = NULL,
    };
    set_spotify_state(APP_SPOTIFY_CONNECTING);
    esp_err_t err = spotify_connect_start(&cfg);   /* copies the strings */
    memset(client_secret, 0, sizeof(client_secret));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spotify_connect_start: %s", esp_err_to_name(err));
        set_spotify_state(APP_SPOTIFY_START_FAILED);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_sp.started = true;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Spotify Connect started as \"%s\", %d kbit/s", name, app_settings_spotify_bitrate());
}

/* ---- net: OTA and setup portal ---------------------------------------------------- */

/* Runs on the OTA worker or the esp_timer task. Keep it short. */
static void on_ota_event(const net_ota_status_t *st, void *ctx)
{
    (void)ctx;
    if (st->state == NET_OTA_REBOOTING) {
        audio_out_stop();   /* mute before the restart: no pop */
    }
    app_ui_ota_event(st);
}

/* Runs on the portal task, the WiFi task or the event loop task. Keep it short. */
static void on_portal_event(const net_portal_event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev->id == NET_PORTAL_EVT_RESTARTING) {
        audio_out_stop();
    }
    app_ui_portal_event(ev);
}

/* After an update the new firmware must confirm itself, or the bootloader
 * rolls it back (net_ota.h): once the network has been up for 30 s and Spotify
 * Connect reached a stable state (logged in, waiting for a login from the app,
 * or not configured) - or after 90 s of network anyway, so that a Spotify
 * outage does not roll back a working firmware. A crash or restart before
 * that rolls back. */
static void confirm_firmware(void)
{
    static bool done;
    static int64_t net_up_since_us;
    if (done) {
        return;
    }
    if (!app_network_up()) {
        net_up_since_us = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (net_up_since_us == 0) {
        net_up_since_us = now;
    }
    app_spotify_info_t sp;
    app_get_spotify_info(&sp);
    bool spotify_ok = sp.state == APP_SPOTIFY_LOGGED_IN || sp.state == APP_SPOTIFY_WAITING_LOGIN ||
                      sp.state == APP_SPOTIFY_NOT_CONFIGURED;
    int64_t up_us = now - net_up_since_us;
    if ((spotify_ok && up_us > 30LL * 1000000) || up_us > 90LL * 1000000) {
        done = net_ota_mark_valid() == ESP_OK;
    }
}

/* Brings up SNTP and Spotify Connect as soon as the preconditions hold.
 * Woken by app_spotify_settings_changed() and polls the network every 2 s. */
static void supervisor_task(void *arg)
{
    (void)arg;
#if CONFIG_MS_NET_QEMU_OPENETH
    s_qemu_net_up = net_qemu_start(pdMS_TO_TICKS(30000)) == ESP_OK;
#endif
    bool sntp_started = false;
    for (;;) {
        if (!sntp_started && app_network_up()) {
            /* Wall-clock time (SNTP, Europe/Berlin) for the UI and the Web API. */
            net_time_start();
            sntp_started = true;
            log_heap("network up");
        }
        app_spotify_info_t info;
        app_get_spotify_info(&info);
        if (!info.started) {
            try_start_spotify();
        }
        confirm_firmware();
        int32_t vol = s_volume_to_save;
        if (vol >= 0) {
            s_volume_to_save = -1;
            if ((uint32_t)vol != settings_get_u32_or(SETTINGS_KEY_VOLUME, UINT16_MAX)) {
                settings_set_u32(SETTINGS_KEY_VOLUME, (uint32_t)vol);
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    }
}

/* ---- NFC ----------------------------------------------------------------------- */

/* Runs on the nfc task. Only logs: playback from a tag comes with the state
 * machine (docs/ARCHITECTURE.md, section 4). */
static void on_nfc_event(const nfc_event_t *ev, void *ctx)
{
    (void)ctx;
    app_ui_nfc_event(ev);
    char uri[SPOTIFY_REF_URI_MAX] = "";
    if (ev->has_ref) {
        spotify_ref_to_uri(&ev->ref, uri, sizeof uri);
    }
    switch (ev->id) {
    case NFC_EVT_TAG_PRESENT:
        if (ev->has_ref) {
            ESP_LOGI(TAG, "nfc: tag present: %s", uri);
        } else {
            ESP_LOGI(TAG, "nfc: tag present without Spotify link (%s)",
                     ev->read_ok ? nfc_tag_err_str(ev->tag_err) : "tag not readable");
        }
        break;
    case NFC_EVT_TAG_WRITTEN:
        ESP_LOGI(TAG, "nfc: tag written: %s", uri);
        break;
    case NFC_EVT_TAG_REMOVED:
        ESP_LOGI(TAG, "nfc: tag removed");
        break;
    case NFC_EVT_READER_OK:
        ESP_LOGI(TAG, "nfc: reader ready");
        break;
    case NFC_EVT_READER_ERROR:
        ESP_LOGW(TAG, "nfc: reader not answering (see \"nfc status\", docs/NFC.md)");
        break;
    }
}

static void start_nfc(void)
{
#if defined(BOARD_NFC_UART_TX_GPIO) && defined(BOARD_NFC_UART_RX_GPIO)
    esp_err_t err = nfc_start(on_nfc_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NFC service not available: %s", esp_err_to_name(err));
    }
#endif
}

/* ---- status LED -------------------------------------------------------------- */

#if BOARD_STATUS_LED_GPIO >= 0
static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        bench_led_mode_t mode = app_spotify_playing() ? BENCH_LED_PLAYING
                                : app_network_up()    ? BENCH_LED_CONNECTED
                                                      : BENCH_LED_NO_WIFI;
        uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
        gpio_set_level((gpio_num_t)BOARD_STATUS_LED_GPIO, bench_led_is_on(mode, ms) ? 0 : 1);   /* active low */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void start_status_led(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "status LED not available");
        return;
    }
    gpio_set_level((gpio_num_t)BOARD_STATUS_LED_GPIO, 1);
    xTaskCreate(led_task, "status_led", 2048, NULL, 1, NULL);
}
#else
static void start_status_led(void)
{
    /* this board has no user LED */
}
#endif

/* ---- entry point --------------------------------------------------------------- */

/* Brownouts and crashes are counted in NVS: on the Marshall (no PC, no log)
 * the console command "status" shows them afterwards. */
static void count_abnormal_reset(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *key = NULL;
    if (r == ESP_RST_BROWNOUT) {
        key = SETTINGS_KEY_RST_BROWNOUT;
    } else if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
        key = SETTINGS_KEY_RST_CRASH;
    }
    if (key) {
        uint32_t n = settings_get_u32_or(key, 0) + 1;
        settings_set_u32(key, n);
        ESP_LOGW(TAG, "abnormal reset (%d), count now %" PRIu32, (int)r, n);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Music Streamer bench firmware on %s, PSRAM %u bytes", BOARD_NAME,
             (unsigned)esp_psram_get_size());
    log_heap("boot");

    ESP_ERROR_CHECK(settings_init());
    count_abnormal_reset();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Display and UI early: the start screen shows while the rest comes up. */
    app_ui_start();

    /* Audio first: the DAC gets clean clocks and is un-muted long before any
     * audio arrives. A failure here is not fatal for the rest of the bench. */
    const audio_out_config_t audio_cfg = AUDIO_OUT_DEFAULT_CONFIG();
    esp_err_t err = audio_out_init(&audio_cfg);
    if (err == ESP_OK) {
        err = audio_out_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio output not available: %s", esp_err_to_name(err));
    }

    esp_err_t cerr = console_start();
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "serial console not available: %s", esp_err_to_name(cerr));
    }
    start_status_led();
    start_nfc();

    net_portal_set_event_cb(on_portal_event, NULL);
#if !CONFIG_MS_NET_QEMU_OPENETH
    char device_name[APP_DEVICE_NAME_MAX];
    app_settings_device_name(device_name, sizeof(device_name));
    const net_wifi_config_t wifi_cfg = {
        .list_networks = app_settings_wifi_list,    /* stored networks + build-time network */
        .device_name = device_name,
    };
    err = net_wifi_start(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not available: %s", esp_err_to_name(err));
    }
#endif
    net_ota_set_busy_cb(app_spotify_playing);
    net_ota_set_event_cb(on_ota_event, NULL);
    err = net_ota_start(CONFIG_MS_OTA_MANIFEST_URL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA updates not available: %s", esp_err_to_name(err));
    }
    xTaskCreate(supervisor_task, "supervisor", 6144, NULL, 4, &s_supervisor);

#if CONFIG_MS_HEAP_LOG_PERIOD_S > 0
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MS_HEAP_LOG_PERIOD_S * 1000));
        audio_out_stats_t st;
        audio_out_get_stats(&st);
        log_heap("periodic");
        ESP_LOGI(TAG, "audio: buffered %" PRIu32 " bytes, played %" PRIu64 " frames, underruns %" PRIu32 "%s%s",
                 st.buffered_bytes, st.frames_played, st.underruns,
                 st.paused ? ", paused" : "", st.muted ? ", muted" : "");
    }
#endif
}
