/*
 * Serial console of the bench firmware.
 *
 * Commands take plain argc/argv (esp_console splits the line and honours
 * "double quotes", so SSIDs with spaces work: wifi add "My Net" secret).
 * Secrets are never printed: passwords not at all, the Spotify client secret
 * only as its last four characters.
 */
#include "console.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_settings.h"
#include "audio_out.h"
#include "bench_util.h"
#include "board_pins.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "nfc.h"
#include "nfc_tag.h"
#include "sdkconfig.h"
#include "settings.h"
#include "net_ota.h"
#include "net_portal.h"
#include "net_wifi.h"
#include "spotify_connect.h"

static const char *TAG = "console";

/* ---- wifi --------------------------------------------------------------------- */

static int wifi_usage(void)
{
    printf("usage:\n"
           "  wifi add <ssid> <password>   (quote SSIDs with spaces: wifi add \"My Net\" secret;\n"
           "                                open network: wifi add <ssid> \"\")\n"
           "  wifi list\n"
           "  wifi del <ssid>\n"
           "  wifi portal [stop]   (setup hotspot with web page)\n");
    return 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        return wifi_usage();
    }
    if (strcmp(argv[1], "list") == 0) {
        settings_wifi_t nets[SETTINGS_WIFI_MAX];
        int n = settings_wifi_list(nets, SETTINGS_WIFI_MAX);
        if (n == 0) {
            printf("no networks stored\n");
        }
        for (int i = 0; i < n; i++) {
            printf("  %d. \"%s\"%s\n", i + 1, nets[i].ssid, nets[i].password[0] ? "" : " (open)");
        }
        net_wifi_info_t info;
        net_wifi_get_info(&info);
        printf("state: %s%s%s\n", net_wifi_state_name(info.state), info.ssid[0] ? ", network " : "",
               info.ssid);
        return 0;
    }
    if (strcmp(argv[1], "portal") == 0) {
        if (argc == 3 && strcmp(argv[2], "stop") == 0) {
            net_portal_stop();
            printf("setup hotspot stops\n");
            return 0;
        }
        if (argc != 2) {
            return wifi_usage();
        }
        esp_err_t e = net_portal_start();
        if (e != ESP_OK) {
            printf("error: %s\n", esp_err_to_name(e));
            return 1;
        }
        char ssid[33], qr[80];
        net_portal_get_ssid(ssid, sizeof(ssid));
        net_portal_get_qr_payload(qr, sizeof(qr));
        printf("setup hotspot \"%s\" (open), page %s\nQR code text: %s\n", ssid, net_portal_get_url(), qr);
        return 0;
    }
    if (strcmp(argv[1], "add") == 0) {
        if (argc != 4) {
            return wifi_usage();
        }
        const char *err = bench_check_ssid(argv[2]);
        if (!err) {
            err = bench_check_wifi_password(argv[3]);
        }
        if (err) {
            printf("error: %s\n", err);
            return 1;
        }
        esp_err_t e = settings_wifi_add(argv[2], argv[3]);
        if (e != ESP_OK) {
            printf("error: could not store the network (%s)\n", esp_err_to_name(e));
            return 1;
        }
        printf("stored \"%s\" (up to %d networks are kept, oldest is dropped)\n", argv[2], SETTINGS_WIFI_MAX);
        net_wifi_rescan();
        return 0;
    }
    if (strcmp(argv[1], "del") == 0) {
        if (argc != 3) {
            return wifi_usage();
        }
        esp_err_t e = settings_wifi_remove(argv[2]);
        if (e == ESP_ERR_NOT_FOUND) {
            printf("no stored network \"%s\"\n", argv[2]);
            return 1;
        }
        if (e != ESP_OK) {
            printf("error: %s\n", esp_err_to_name(e));
            return 1;
        }
        printf("removed \"%s\" (an active connection stays up until it drops)\n", argv[2]);
        return 0;
    }
    return wifi_usage();
}

/* ---- spotify ------------------------------------------------------------------- */

static int spotify_usage(void)
{
    printf("usage:\n"
           "  spotify creds <client_id> <client_secret>   (from developer.spotify.com/dashboard)\n"
           "  spotify forget   (delete the stored Spotify login; takes effect after reboot)\n"
           "  spotify          (show state)\n");
    return 1;
}

static void print_spotify_state(void)
{
    char id[APP_CREDENTIAL_MAX], secret[APP_CREDENTIAL_MAX], masked[16];
    app_setting_source_t src = app_settings_spotify_credentials(id, sizeof(id), secret, sizeof(secret));
    bench_mask_secret(secret, masked, sizeof(masked));
    memset(secret, 0, sizeof(secret));
    printf("credentials: %s", app_setting_source_name(src));
    if (src != APP_SETTING_UNSET) {
        printf(" (client id %s, secret %s)", id, masked);
    }
    printf("\n");

    app_spotify_info_t sp;
    app_get_spotify_info(&sp);
    printf("state:       %s\n", app_spotify_state_name(sp.state));
    if (sp.user[0]) {
        printf("user:        %s (%s)\n", sp.user, sp.stored_credentials ? "stored login" : "zeroconf login");
    }
    printf("playback:    %s%s%s\n", app_spotify_playing() ? "playing" : "not playing",
           sp.track[0] ? ", last track: " : "", sp.track);
    if (sp.started) {
        spotify_connect_status_t st;
        spotify_connect_get_status(&st);
        printf("connect:     logged_in=%d active=%d paused=%d position=%" PRIu32 "/%" PRIu32 " ms\n",
               st.logged_in, st.active, st.paused, st.position_ms, st.duration_ms);
    }
}

static int cmd_spotify(int argc, char **argv)
{
    if (argc == 1) {
        print_spotify_state();
        return 0;
    }
    if (strcmp(argv[1], "creds") == 0) {
        if (argc != 4) {
            return spotify_usage();
        }
        const char *err = bench_check_credential(argv[2]);
        if (!err) {
            err = bench_check_credential(argv[3]);
        }
        if (err) {
            printf("error: %s\n", err);
            return 1;
        }
        if (!bench_is_hex32(argv[2]) || !bench_is_hex32(argv[3])) {
            printf("warning: Spotify client id and secret are normally 32 hex digits - check for typos\n");
        }
        esp_err_t e = settings_set_str(SETTINGS_KEY_SP_CLIENT_ID, argv[2]);
        if (e == ESP_OK) {
            e = settings_set_str(SETTINGS_KEY_SP_CLIENT_SECRET, argv[3]);
        }
        memset(argv[3], 0, strlen(argv[3]));
        if (e != ESP_OK) {
            printf("error: could not store the credentials (%s)\n", esp_err_to_name(e));
            return 1;
        }
        app_spotify_info_t sp;
        app_get_spotify_info(&sp);
        if (sp.started) {
            printf("stored. Spotify Connect is already running: reboot to use the new credentials.\n");
        } else {
            printf("stored. Spotify Connect starts as soon as WiFi is up.\n");
            app_spotify_settings_changed();
        }
        return 0;
    }
    if (strcmp(argv[1], "forget") == 0) {
        esp_err_t e = spotify_connect_forget_credentials();
        printf("%s\n", e == ESP_OK ? "stored Spotify login deleted; reboot, then pick the device in the app again"
                                   : esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    return spotify_usage();
}

/* ---- name ---------------------------------------------------------------------- */

static int cmd_name(int argc, char **argv)
{
    char name[APP_DEVICE_NAME_MAX];
    if (argc < 2) {
        app_setting_source_t src = app_settings_device_name(name, sizeof(name));
        printf("device name: \"%s\" (%s)\nusage: name <new name>\n", name, app_setting_source_name(src));
        return 0;
    }
    if (!bench_join_args(argc - 1, argv + 1, name, sizeof(name))) {
        printf("error: name too long (max %d bytes)\n", APP_DEVICE_NAME_MAX - 1);
        return 1;
    }
    const char *err = bench_check_device_name(name);
    if (err) {
        printf("error: %s\n", err);
        return 1;
    }
    esp_err_t e = settings_set_str(SETTINGS_KEY_DEVICE_NAME, name);
    if (e != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(e));
        return 1;
    }
    printf("device name set to \"%s\" - reboot to apply it in the Spotify app\n", name);
    return 0;
}

/* ---- tone ---------------------------------------------------------------------- */

static int cmd_tone(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        audio_out_tone_stop();
        printf("tone stopped\n");
        return 0;
    }
    if (app_spotify_playing()) {
        printf("Spotify is playing on this device - pause it in the app first\n");
        return 1;
    }
    bench_tone_args_t t;
    if (argc >= 2 && strcmp(argv[1], "noise-test") == 0) {
        t.freq_hz = BENCH_NOISE_TEST_FREQ_HZ;
        t.dbfs = BENCH_NOISE_TEST_DBFS;
        t.seconds = BENCH_NOISE_TEST_SECONDS;
    } else {
        const char *err = bench_parse_tone(argc - 1, argv + 1, &t);
        if (err) {
            printf("error: %s\nusage: tone [freq_hz] [dbfs] [seconds] | tone noise-test | tone stop\n", err);
            return 1;
        }
    }
    esp_err_t e = audio_out_tone_start(t.freq_hz, t.dbfs, (uint32_t)(t.seconds * 1000.0f));
    if (e != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(e));
        return 1;
    }
    printf("tone: %.0f Hz, %.1f dBFS, %.0f s (\"tone stop\" ends it)\n", t.freq_hz, t.dbfs, t.seconds);
    return 0;
}

/* ---- ota ----------------------------------------------------------------------- */

static void print_ota_state(void)
{
    net_ota_status_t st;
    net_ota_get_status(&st);
    printf("update:      %s", net_ota_state_name(st.state));
    if (st.state == NET_OTA_DOWNLOADING) {
        printf(" %u %%", st.progress);
    }
    if (st.available_version[0]) {
        printf(", offered %s", st.available_version);
    }
    if (st.last_error[0]) {
        printf(" (%s)", st.last_error);
    }
    if (st.pending_verify) {
        printf(", running firmware not confirmed yet");
    }
    if (st.rolled_back) {
        printf(", %s was rolled back", st.rolled_back_version);
    }
    printf("\n");
    if (st.notes[0]) {
        printf("             notes: %s\n", st.notes);
    }
}

static int ota_usage(void)
{
    printf("usage:\n"
           "  ota              (state)\n"
           "  ota check        (look for a newer version, do not install)\n"
           "  ota install      (install a newer version and restart)\n"
           "  ota url [<https url> | default]   (manifest URL)\n");
    return 1;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc == 1) {
        char url[256];
        bool from_nvs = false;
        net_ota_get_url(url, sizeof(url), &from_nvs);
        printf("firmware:    %s\n", esp_app_get_description()->version);
        printf("manifest:    %s%s\n", url[0] ? url : "(none)", from_nvs ? " (set with ota url)" : "");
        print_ota_state();
        return 0;
    }
    if (strcmp(argv[1], "check") == 0 || strcmp(argv[1], "install") == 0) {
        bool install = argv[1][0] == 'i';
        esp_err_t e = install ? net_ota_install() : net_ota_check_now();
        if (e != ESP_OK) {
            printf("error: %s (no manifest URL, or a check is already running)\n", esp_err_to_name(e));
            return 1;
        }
        printf("%s - see \"ota\" for the result\n", install ? "installing if newer" : "checking");
        return 0;
    }
    if (strcmp(argv[1], "url") == 0 && argc <= 3) {
        if (argc == 2) {
            return cmd_ota(1, argv);
        }
        esp_err_t e = net_ota_set_url(strcmp(argv[2], "default") == 0 ? NULL : argv[2]);
        if (e != ESP_OK) {
            printf("error: %s\n", e == ESP_ERR_INVALID_ARG ? "not a valid https:// URL" : esp_err_to_name(e));
            return 1;
        }
        char url[256];
        net_ota_get_url(url, sizeof(url), NULL);
        printf("manifest URL: %s\n", url[0] ? url : "(none, updates disabled)");
        return 0;
    }
    return ota_usage();
}

/* ---- nfc ----------------------------------------------------------------------- */

#define NFC_WRITE_TIMEOUT_MS 15000
#define NFC_READ_TIMEOUT_MS  5000

static int nfc_usage(void)
{
    printf("usage:\n"
           "  nfc [status]          reader and tag state\n"
           "  nfc read              read the tag on the reader (waits %d s)\n"
           "  nfc write <link|uri>  write a Spotify link, e.g. https://open.spotify.com/playlist/<id>\n"
           "                        or spotify:album:<id> (waits %d s for a tag)\n"
           "  nfc poll <ms>         poll interval (50..5000, default 300)\n",
           NFC_READ_TIMEOUT_MS / 1000, NFC_WRITE_TIMEOUT_MS / 1000);
    return 1;
}

static void print_uid(const uint8_t *uid, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%s%02X", i ? ":" : "", uid[i]);
    }
}

static void print_ref(const spotify_ref_t *ref)
{
    char uri[SPOTIFY_REF_URI_MAX], url[SPOTIFY_REF_URL_MAX];
    spotify_ref_to_uri(ref, uri, sizeof uri);
    spotify_ref_to_url(ref, url, sizeof url);
    printf("%s (%s)", uri, url);
}

static void print_nfc_state(bool verbose)
{
    nfc_status_t st;
    nfc_get_status(&st);
    if (!st.running) {
        printf("nfc:         not running\n");
        return;
    }
    printf("nfc:         reader %s", st.reader_ok ? "ready" : "NOT answering");
    if (st.firmware[0]) {
        printf(" (%s)", st.firmware);
    }
    printf(", tag ");
    if (st.tag_present) {
        print_uid(st.uid, st.uid_len);
        if (st.model) {
            printf(" NTAG%u", st.model);
        }
        printf(": ");
        if (st.has_ref) {
            print_ref(&st.ref);
        } else {
            printf("no Spotify link (%s)", st.tag_error);
        }
    } else {
        printf("none");
    }
    printf("\n");
    if (!verbose) {
        return;
    }
    printf("polling:     every %" PRIu32 " ms, %" PRIu32 " polls\n", st.poll_interval_ms, st.polls);
    printf("counters:    tags %" PRIu32 ", tag read errors %" PRIu32 ", reader errors %" PRIu32
           ", reader inits %" PRIu32 ", writes %" PRIu32 " ok / %" PRIu32 " failed\n",
           st.tags_detected, st.read_errors, st.comm_errors, st.reader_inits, st.writes_ok, st.writes_failed);
    if (st.last_error[0]) {
        printf("last error:  %s\n", st.last_error);
    }
    if (st.last_write_error[0]) {
        printf("last write:  %s\n", st.last_write_error);
    }
    if (!st.reader_ok) {
        printf("hint:        wiring and DIP switches: docs/NFC.md\n");
    }
}

static int cmd_nfc(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        print_nfc_state(true);
        return 0;
    }
    if (strcmp(argv[1], "read") == 0 && argc == 2) {
        nfc_read_result_t *r = calloc(1, sizeof *r);
        if (!r) {
            printf("error: out of memory\n");
            return 1;
        }
        printf("hold a tag on the reader...\n");
        esp_err_t e = nfc_read_now(r, NFC_READ_TIMEOUT_MS);
        int ret = 1;
        if (e == ESP_OK || (e == ESP_FAIL && r->uid_len)) {
            printf("tag:   ");
            print_uid(r->uid, r->uid_len);
            printf(", SAK %02X", r->sak);
            if (r->model) {
                printf(", NTAG%u", r->model);
            }
            printf(", %u data bytes read\n", (unsigned)r->bytes_read);
            if (r->has_ref) {
                printf("link:  ");
                print_ref(&r->ref);
                printf("\n");
                ret = 0;
            } else {
                printf("error: %s\n", r->error);
                if (r->link[0]) {
                    printf("found: %s\n", r->link);
                }
            }
        } else {
            printf("error: %s (%s)\n", r->error[0] ? r->error : "read failed", esp_err_to_name(e));
        }
        free(r);
        return ret;
    }
    if (strcmp(argv[1], "write") == 0 && argc == 3) {
        spotify_ref_t ref;
        nfc_tag_err_t pe = nfc_tag_parse_link_ex(argv[2], &ref);
        if (pe != NFC_TAG_OK) {
            printf("error: %s\n", nfc_tag_err_str(pe));
            return 1;
        }
        printf("writing ");
        print_ref(&ref);
        printf("\nhold a tag on the reader (%d s)...\n", NFC_WRITE_TIMEOUT_MS / 1000);
        esp_err_t e = nfc_write_ref(&ref, NFC_WRITE_TIMEOUT_MS);
        if (e == ESP_OK) {
            printf("written and verified\n");
            return 0;
        }
        nfc_status_t st;
        nfc_get_status(&st);
        printf("error: %s (%s)\n", st.last_write_error, esp_err_to_name(e));
        return 1;
    }
    if (strcmp(argv[1], "poll") == 0 && argc == 3) {
        char *end = NULL;
        long ms = strtol(argv[2], &end, 10);
        if (!end || *end || ms < 50 || ms > 5000) {
            printf("error: 50..5000 ms\n");
            return 1;
        }
        nfc_set_poll_interval_ms((uint32_t)ms);
        printf("poll interval %ld ms\n", ms);
        return 0;
    }
    return nfc_usage();
}

/* ---- status -------------------------------------------------------------------- */

static const char *reset_reason_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart (reboot, update)";
    case ESP_RST_PANIC:     return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage too low)";
    case ESP_RST_USB:       return "USB (flashing / serial monitor)";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:                return "other";
    }
}

static int cmd_status(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        settings_set_u32(SETTINGS_KEY_RST_BROWNOUT, 0);
        settings_set_u32(SETTINGS_KEY_RST_CRASH, 0);
        printf("reset counters cleared\n");
        return 0;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    printf("firmware:    %s %s (IDF %s), board %s\n", app->project_name, app->version, app->idf_ver, BOARD_NAME);
    printf("uptime:      %" PRIu32 " s, last reset: %s\n", (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ),
           reset_reason_text(esp_reset_reason()));
    printf("resets:      %" PRIu32 " brownout(s), %" PRIu32 " crash(es) since the counters were last cleared"
           " (\"status clear\")\n",
           settings_get_u32_or(SETTINGS_KEY_RST_BROWNOUT, 0), settings_get_u32_or(SETTINGS_KEY_RST_CRASH, 0));
    printf("heap:        internal free %u (min %u, largest %u), psram free %u (min %u)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    net_wifi_info_t w;
    net_wifi_get_info(&w);
    esp_ip4_addr_t ip = { .addr = app_network_up() ? app_network_ip() : 0 };
    printf("network:     %s", app_network_up() ? "up" : "down");
    printf(", wifi %s", net_wifi_state_name(w.state));
    if (w.ssid[0]) {
        printf(" \"%s\"", w.ssid);
    }
    if (w.state == NET_WIFI_CONNECTED) {
        printf(", rssi %d dBm, channel %u", w.rssi, w.channel);
    }
    if (ip.addr) {
        printf(", ip " IPSTR, IP2STR(&ip));
    }
    if (w.last_reason) {
        printf(", last disconnect reason %u", w.last_reason);
    }
    printf("\n");
    if (net_portal_is_active()) {
        char ssid[33];
        net_portal_get_ssid(ssid, sizeof(ssid));
        printf("setup:       hotspot \"%s\" active, %d phone(s), %s\n", ssid, net_portal_client_count(),
               net_portal_get_url());
    }
    print_ota_state();

    char name[APP_DEVICE_NAME_MAX];
    app_settings_device_name(name, sizeof(name));
    printf("device name: \"%s\", %d kbit/s\n", name, app_settings_spotify_bitrate());
    print_spotify_state();

    audio_out_stats_t a;
    audio_out_get_stats(&a);
    printf("audio:       %s%s%s, buffered %" PRIu32 " B, played %" PRIu64 " frames, dropped %" PRIu64
           ", underruns %" PRIu32 ", dma gaps %" PRIu32 " (late but ok %" PRIu32 ")%s\n",
           a.running ? "running" : "stopped", a.paused ? ", paused" : "", a.muted ? ", MUTED" : "",
           a.buffered_bytes, a.frames_played, a.frames_dropped, a.underruns, a.dma_underruns, a.dma_late,
           audio_out_tone_active() ? ", test tone active" : "");
    print_nfc_state(false);
    return 0;
}

/* ---- reboot / factory-reset ------------------------------------------------------- */

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    audio_out_stop();   /* mute first: no pop */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("erases WiFi networks, Spotify credentials and the stored Spotify login.\n"
               "type: factory-reset yes\n");
        return 1;
    }
    esp_err_t e = settings_factory_reset();
    if (e != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(e));
        return 1;
    }
    printf("all settings erased\n");
    return cmd_reboot(0, NULL);
}

/* ---- registration -------------------------------------------------------------- */

static esp_err_t register_cmd(const char *name, const char *help, const char *hint,
                              esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = func,
    };
    return esp_console_cmd_register(&cmd);
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "streamer>";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 6144;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
#else
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(register_cmd("wifi", "Manage WiFi networks", "add <ssid> <password> | list | del <ssid> | portal [stop]", cmd_wifi));
    ESP_ERROR_CHECK(register_cmd("ota", "Firmware updates", "[check | install | url <url>|default]", cmd_ota));
    ESP_ERROR_CHECK(register_cmd("spotify", "Spotify credentials and state", "[creds <id> <secret> | forget]", cmd_spotify));
    ESP_ERROR_CHECK(register_cmd("name", "Show or set the Spotify Connect device name", "[new name]", cmd_name));
    ESP_ERROR_CHECK(register_cmd("tone", "Test tone on the DAC (defaults 1000 Hz, -20 dBFS, 5 s)",
                                 "[freq_hz] [dbfs] [seconds] | noise-test | stop", cmd_tone));
    ESP_ERROR_CHECK(register_cmd("nfc", "NFC reader: state, read a tag, write a Spotify link to a tag",
                                 "[status | read | write <link> | poll <ms>]", cmd_nfc));
    ESP_ERROR_CHECK(register_cmd("status", "Firmware, memory, network, Spotify and audio state; \"status clear\" resets the reset counters", "[clear]", cmd_status));
    ESP_ERROR_CHECK(register_cmd("reboot", "Restart the device", NULL, cmd_reboot));
    ESP_ERROR_CHECK(register_cmd("factory-reset", "Erase all settings", "yes", cmd_factory_reset));

    err = esp_console_start_repl(repl);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "serial console ready - type \"help\"");
    }
    return err;
}
