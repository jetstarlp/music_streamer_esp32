/*
 * WiFi station (moved here from main/wifi_sta.c; the connection strategy is
 * unchanged) plus the hooks for the setup portal: connection requests from
 * the portal page, scans, the automatic portal start, and attempts limited to
 * the hotspot's channel while a phone uses the hotspot.
 */
#include "net_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_internal.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "net_wifi";

#define BIT_CONNECTED           BIT0    /* IP address assigned                   */
#define BIT_DISCONNECTED        BIT1    /* STA_DISCONNECTED since the last clear */
#define BIT_REQUEST             BIT2    /* the portal asked for a network        */

/* scan + association + DHCP, per network. DHCP took 17 s at -85 dBm on the
 * bench; 20 s cut it off and started over. */
#define CONNECT_TIMEOUT_MS      45000
#define SAME_NETWORK_RETRIES    3       /* after a connection loss                */
#define RETRY_DELAY_MS          2000
/* Pause after every network failed: short at first (a weak signal often loses
 * just one authentication frame, reason 2 AUTH_EXPIRE), 15 s in the long run. */
static const uint16_t ROUND_PAUSE_MS[] = {1000, 3000, 5000, 10000, 15000};
#define ROUND_PAUSE_STEPS       (sizeof(ROUND_PAUSE_MS) / sizeof(ROUND_PAUSE_MS[0]))
/* Driver-internal retries of the same access point before it reports failure. */
#define DRIVER_RETRIES          3
#define MAX_CANDIDATES          (SETTINGS_WIFI_MAX + 1)   /* + e.g. a build-time network */
/* While a phone is on the hotspot: look for stored networks on its channel. */
#define QUIET_PERIOD_MS         15000
#define SCAN_MAX                24
#define YIELD_POLL_MS           250

/* Internal-RAM stack on purpose: settings_wifi_add/mark_used write to flash. */
#define TASK_STACK              4096
#define TASK_PRIO               5
#define TASK_CORE               0

typedef enum {
    TRY_BACKGROUND,     /* stored network, gives way to scans and portal requests */
    TRY_REQUESTED,      /* entered on the portal page                             */
} try_kind_t;

static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_radio;
static esp_timer_handle_t s_portal_timer;
static int (*s_list)(settings_wifi_t *out, int max);
static char s_device_name[64];

static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;
static net_wifi_info_t s_info;          /* rssi/channel are filled in on demand */
static settings_wifi_t s_req;           /* guarded by s_info_mux */
static bool s_req_pending;
static net_attempt_t s_attempt;
static volatile int s_yield;            /* tasks waiting for the radio */
static uint8_t s_fail_reason;           /* of the last failed try_connect (wifi task only) */

static void set_state(net_wifi_state_t state, const char *ssid)
{
    portENTER_CRITICAL(&s_info_mux);
    s_info.state = state;
    if (ssid) {
        strlcpy(s_info.ssid, ssid, sizeof(s_info.ssid));
    }
    portEXIT_CRITICAL(&s_info_mux);
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password?";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "security mode not supported (needs WPA2/WPA3 or open)";
    case WIFI_REASON_AUTH_EXPIRE:
        return "router did not answer, weak signal?";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    case WIFI_REASON_ASSOC_LEAVE:
        return "disconnected by this device";
    case NET_REASON_NO_IP:
        return "no IP address from the router";
    default:
        return "see wifi_err_reason_t";
    }
}

const char *net_wifi_reason_text_de(uint8_t reason)
{
    switch (reason) {
    case NET_REASON_UNAVAILABLE:
        return "WLAN ist auf diesem Gerät nicht verfügbar.";
    case NET_REASON_NO_IP:
        return "Der Router hat keine Verbindung zugelassen (keine IP-Adresse innerhalb von 20 Sekunden). "
               "Bitte noch einmal versuchen.";
    case WIFI_REASON_NO_AP_FOUND:
        return "WLAN nicht gefunden. Ist der Name richtig geschrieben und der Router in Reichweite? "
               "(Nur 2,4-GHz-WLANs werden unterstützt.)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Passwort falsch? Bitte noch einmal eingeben (Groß-/Kleinschreibung beachten).";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "Die Verschlüsselung passt nicht: Das WLAN braucht WPA2/WPA3, oder es ist offen und "
               "das Passwortfeld muss leer bleiben.";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "Der Router antwortet nicht. Signal zu schwach? Näher an den Router gehen.";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Die Verbindung ist abgerissen. Signal zu schwach?";
    default:
        return "Die Verbindung ist fehlgeschlagen. Bitte noch einmal versuchen.";
    }
}

static void finish_attempt(const char *ssid, net_attempt_state_t state, uint8_t reason, uint32_t ip)
{
    bool mine = false;
    net_attempt_t copy;
    portENTER_CRITICAL(&s_info_mux);
    if (s_attempt.state == NET_ATTEMPT_RUNNING && strcmp(s_attempt.ssid, ssid) == 0) {
        s_attempt.state = state;
        s_attempt.reason = reason;
        s_attempt.ip = ip;
        copy = s_attempt;
        mine = true;
    }
    portEXIT_CRITICAL(&s_info_mux);
    if (mine) {
        net_portal_attempt_done(&copy);
    }
}

static void arm_portal_timer(void)
{
#if CONFIG_MS_NET_PORTAL_AUTO_S > 0
    if (s_portal_timer && !esp_timer_is_active(s_portal_timer)) {
        esp_timer_start_once(s_portal_timer, (uint64_t)CONFIG_MS_NET_PORTAL_AUTO_S * 1000000ULL);
    }
#endif
}

static void on_portal_timer(void *arg)
{
    (void)arg;
    if (!net_wifi_is_connected()) {
        ESP_LOGW(TAG, "no WiFi connection for %d s: starting the setup hotspot", CONFIG_MS_NET_PORTAL_AUTO_S);
        net_portal_auto_start();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        bool was_connected;
        portENTER_CRITICAL(&s_info_mux);
        s_info.ip = 0;
        s_info.last_reason = ev->reason;
        was_connected = s_info.state == NET_WIFI_CONNECTED;
        if (was_connected) {
            s_info.state = NET_WIFI_CONNECTING;     /* the task reconnects */
        }
        portEXIT_CRITICAL(&s_info_mux);
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        if (was_connected) {
            arm_portal_timer();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        portENTER_CRITICAL(&s_info_mux);
        s_info.ip = ev->ip_info.ip.addr;
        portEXIT_CRITICAL(&s_info_mux);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        if (s_portal_timer) {
            esp_timer_stop(s_portal_timer);
        }
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

/* ---- radio lock ---------------------------------------------------------------- */

bool net_wifi_radio_lock(TickType_t timeout)
{
    if (s_radio == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_info_mux);
    s_yield++;
    portEXIT_CRITICAL(&s_info_mux);
    bool ok = xSemaphoreTake(s_radio, timeout) == pdTRUE;
    portENTER_CRITICAL(&s_info_mux);
    s_yield--;
    portEXIT_CRITICAL(&s_info_mux);
    return ok;
}

void net_wifi_radio_unlock(void)
{
    xSemaphoreGive(s_radio);
}

/* ---- connection attempts --------------------------------------------------------- */

/*
 * One connection attempt; blocks until connected, failed or timed out.
 * channel 0: scan all channels and take the strongest access point (the
 * proven default); 1..13: only this channel (while a phone is on the hotspot).
 * A background attempt gives way to portal requests and scans.
 */
static bool try_connect(const settings_wifi_t *net, try_kind_t kind, uint8_t channel)
{
    const bool background = kind == TRY_BACKGROUND;
    if (background) {
        /* A scan or mode change is waiting: let it go first (a mutex is not
         * handed over to a waiting task of lower priority). */
        while (s_yield > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.sta.ssid, net->ssid, strnlen(net->ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, net->password, strnlen(net->password, sizeof(cfg.sta.password)));
    /* Accept open networks only when no password is stored. */
    cfg.sta.threshold.authmode = net->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (channel == 0) {
        /* Mesh systems / repeaters: pick the strongest access point. */
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    } else {
        /* The network was just seen on this channel: connect without leaving it. */
        cfg.sta.channel = channel;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
    }
    cfg.sta.failure_retry_cnt = DRIVER_RETRIES;

    xSemaphoreTake(s_radio, portMAX_DELAY);
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_DISCONNECTED);
    set_state(NET_WIFI_CONNECTING, net->ssid);
    if (channel) {
        ESP_LOGI(TAG, "connecting to \"%s\" on channel %u", net->ssid, channel);
    } else {
        ESP_LOGI(TAG, "connecting to \"%s\"%s", net->ssid, background ? "" : " (entered on the setup page)");
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect \"%s\": %s", net->ssid, esp_err_to_name(err));
        s_fail_reason = NET_REASON_UNAVAILABLE;
        xSemaphoreGive(s_radio);
        return false;
    }

    const EventBits_t wait = BIT_CONNECTED | BIT_DISCONNECTED | (background ? BIT_REQUEST : 0);
    const TickType_t slice = background ? pdMS_TO_TICKS(YIELD_POLL_MS) : pdMS_TO_TICKS(CONNECT_TIMEOUT_MS);
    const TickType_t start = xTaskGetTickCount();
    EventBits_t bits = 0;
    bool yielded = false;
    while (xTaskGetTickCount() - start < pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
        bits = xEventGroupWaitBits(s_events, wait, pdFALSE, pdFALSE, slice);
        if (bits & wait) {
            break;
        }
        if (background && s_yield > 0) {
            yielded = true;
            break;
        }
    }
    if (bits & BIT_CONNECTED) {
        set_state(NET_WIFI_CONNECTED, net->ssid);
        xSemaphoreGive(s_radio);
        return true;
    }
    if (bits & BIT_DISCONNECTED) {
        uint8_t reason = s_info.last_reason;
        s_fail_reason = reason;
        ESP_LOGW(TAG, "\"%s\" failed: reason %u (%s)", net->ssid, reason, reason_text(reason));
    } else if (bits & BIT_REQUEST) {
        s_fail_reason = WIFI_REASON_ASSOC_LEAVE;
        ESP_LOGI(TAG, "\"%s\": attempt cancelled for the network entered on the setup page", net->ssid);
    } else if (yielded) {
        s_fail_reason = WIFI_REASON_ASSOC_LEAVE;
        ESP_LOGI(TAG, "\"%s\": attempt paused for a scan", net->ssid);
    } else {
        s_fail_reason = NET_REASON_NO_IP;
        ESP_LOGW(TAG, "\"%s\" failed: no IP address within %d s", net->ssid, CONNECT_TIMEOUT_MS / 1000);
    }
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));     /* let the resulting event pass */
    xEventGroupClearBits(s_events, BIT_DISCONNECTED);
    xSemaphoreGive(s_radio);
    return false;
}

static bool pop_request(settings_wifi_t *out)
{
    bool have;
    portENTER_CRITICAL(&s_info_mux);
    have = s_req_pending;
    if (have) {
        *out = s_req;
        memset(&s_req, 0, sizeof(s_req));
        s_req_pending = false;
    }
    portEXIT_CRITICAL(&s_info_mux);
    xEventGroupClearBits(s_events, BIT_REQUEST);
    return have;
}

static bool request_pending(void)
{
    return (xEventGroupGetBits(s_events) & BIT_REQUEST) != 0;
}

/* The network entered on the portal page; true = connected (and stored). */
static bool handle_request(settings_wifi_t *req)
{
    if (!try_connect(req, TRY_REQUESTED, 0)) {
        finish_attempt(req->ssid, NET_ATTEMPT_FAILED, s_fail_reason, 0);
        return false;
    }
    esp_err_t err = settings_wifi_add(req->ssid, req->password);   /* also moves it to the front */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "storing \"%s\" failed: %s", req->ssid, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "\"%s\" stored", req->ssid);
    }
    finish_attempt(req->ssid, NET_ATTEMPT_OK, 0, s_info.ip);
    return true;
}

/* Channel for background attempts: 0 (all) normally, the hotspot's channel
 * while a phone is connected to it. */
static uint8_t background_channel(void)
{
    return net_portal_wants_quiet() ? net_portal_channel() : 0;
}

/* A phone is on the hotspot: try only the stored networks that are visible on
 * the hotspot's channel. Returns the index of the connected one, or -1. */
static int quiet_round(settings_wifi_t *nets, int n)
{
    const uint8_t ch = net_portal_channel();
    if (ch == 0) {
        return -1;
    }
    net_ap_info_t *aps = heap_caps_malloc(sizeof(net_ap_info_t) * SCAN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (aps == NULL) {
        aps = malloc(sizeof(net_ap_info_t) * SCAN_MAX);
    }
    if (aps == NULL) {
        return -1;
    }
    size_t found = 0;
    int idx = -1;
    if (net_wifi_scan(ch, aps, SCAN_MAX, &found, pdMS_TO_TICKS(1000)) == ESP_OK) {
        for (int i = 0; i < n && idx < 0 && !request_pending(); i++) {
            if (net_scan_find(aps, found, nets[i].ssid) >= 0 && try_connect(&nets[i], TRY_BACKGROUND, ch)) {
                idx = i;
            }
        }
    }
    free(aps);
    return idx;
}

static void wifi_task(void *arg)
{
    (void)arg;
    /* ~700 bytes incl. passwords: keep them off the small stack */
    settings_wifi_t *nets = calloc(MAX_CANDIDATES + 1, sizeof(settings_wifi_t));
    if (nets == NULL) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
        return;
    }
    settings_wifi_t *current = &nets[MAX_CANDIDATES];

    unsigned round = 0;     /* failed rounds in a row */
    for (;;) {
        bool have = false;
        bool from_request = false;
        if (pop_request(current)) {
            have = handle_request(current);
            from_request = have;
        }
        if (!have) {
            int n = s_list(nets, MAX_CANDIDATES);
            if (n == 0) {
                set_state(NET_WIFI_NO_NETWORKS, "");
                ESP_LOGW(TAG, "no WiFi network configured: starting the setup hotspot");
                net_portal_auto_start();
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }

            int idx = -1;
            const bool quiet = net_portal_wants_quiet();
            if (quiet) {
                idx = quiet_round(nets, n);
            } else {
                for (int i = 0; i < n && idx < 0 && !request_pending(); i++) {
                    if (try_connect(&nets[i], TRY_BACKGROUND, 0)) {
                        idx = i;
                    }
                }
            }
            if (idx < 0) {
                memset(nets, 0, sizeof(settings_wifi_t) * MAX_CANDIDATES);
                if (request_pending()) {
                    continue;
                }
                uint32_t pause;
                if (quiet) {
                    pause = QUIET_PERIOD_MS;
                } else {
                    pause = ROUND_PAUSE_MS[round < ROUND_PAUSE_STEPS ? round : ROUND_PAUSE_STEPS - 1];
                    round++;
                    ESP_LOGW(TAG, "none of %d network(s) reachable, retrying in %u ms", n, (unsigned)pause);
                }
                set_state(NET_WIFI_WAITING, NULL);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pause));
                continue;
            }
            *current = nets[idx];
            memset(nets, 0, sizeof(settings_wifi_t) * MAX_CANDIDATES);
        }
        round = 0;

        if (!from_request) {
            /* Remember it as most recently used. ESP_ERR_NOT_FOUND: e.g. the
             * build-time network is not in the stored list, which is fine. */
            esp_err_t err = settings_wifi_mark_used(current->ssid);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "settings_wifi_mark_used: %s", esp_err_to_name(err));
            }
        }

        /* Stay on this network; after a loss retry it a few times. */
        int retries = 0;
        while (retries < SAME_NETWORK_RETRIES) {
            EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DISCONNECTED | BIT_REQUEST, pdFALSE, pdFALSE,
                                                   portMAX_DELAY);
            if (bits & BIT_REQUEST) {
                ESP_LOGI(TAG, "leaving \"%s\" for the network entered on the setup page", current->ssid);
                xEventGroupClearBits(s_events, BIT_DISCONNECTED);
                esp_wifi_disconnect();
                xEventGroupWaitBits(s_events, BIT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                break;
            }
            xEventGroupClearBits(s_events, BIT_DISCONNECTED);
            ESP_LOGW(TAG, "connection to \"%s\" lost (reason %u, %s)", current->ssid,
                     s_info.last_reason, reason_text(s_info.last_reason));
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            if (request_pending()) {
                break;
            }
            retries = try_connect(current, TRY_BACKGROUND, background_channel()) ? 0 : retries + 1;
        }
        if (retries >= SAME_NETWORK_RETRIES) {
            ESP_LOGW(TAG, "giving up on this network, trying all configured networks");
        }
        memset(current, 0, sizeof(*current));
    }
}

esp_err_t net_wifi_start(const net_wifi_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_events == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_list = (cfg && cfg->list_networks) ? cfg->list_networks : settings_wifi_list;
    strlcpy(s_device_name, (cfg && cfg->device_name) ? cfg->device_name : "", sizeof(s_device_name));
    s_radio = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_radio, ESP_ERR_NO_MEM, TAG, "mutex");
    EventGroupHandle_t events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(events, ESP_ERR_NO_MEM, TAG, "event group");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init");
    s_events = events;
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                            on_event, NULL, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event,
                                                            NULL, NULL),
                        TAG, "ip handler");

    /* Credentials live in the settings component: the driver must not keep
     * its own copy in NVS. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    /* Modem sleep adds up to a DTIM interval of latency to every packet and
     * causes audible drop-outs with a streaming TCP connection: switch it off. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps");

#if CONFIG_MS_NET_PORTAL_AUTO_S > 0
    const esp_timer_create_args_t targs = {.callback = on_portal_timer, .name = "net_portal_auto"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_portal_timer), TAG, "timer");
    arm_portal_timer();
#endif

    BaseType_t ok = xTaskCreatePinnedToCore(wifi_task, "wifi_sta", TASK_STACK, NULL, TASK_PRIO,
                                            &s_task, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

bool net_wifi_wait_connected(TickType_t timeout)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, timeout) & BIT_CONNECTED) != 0;
}

bool net_wifi_is_connected(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool net_wifi_started(void)
{
    return s_events != NULL;
}

const char *net_wifi_device_name(void)
{
    return s_device_name;
}

size_t net_wifi_known_ssids(char (*out)[33], size_t max)
{
    settings_wifi_t *nets = calloc(SETTINGS_WIFI_MAX, sizeof(settings_wifi_t));
    if (nets == NULL) {
        return 0;
    }
    int n = settings_wifi_list(nets, SETTINGS_WIFI_MAX);
    size_t k = 0;
    for (int i = 0; i < n && k < max; i++) {
        strlcpy(out[k++], nets[i].ssid, 33);
    }
    memset(nets, 0, sizeof(settings_wifi_t) * SETTINGS_WIFI_MAX);
    free(nets);
    return k;
}

void net_wifi_get_info(net_wifi_info_t *out)
{
    portENTER_CRITICAL(&s_info_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_info_mux);
    out->rssi = 0;
    out->channel = 0;
    if (out->state == NET_WIFI_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
            out->channel = ap.primary;
        }
    }
}

void net_wifi_rescan(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t net_wifi_request(const char *ssid, const char *password)
{
    portENTER_CRITICAL(&s_info_mux);
    memset(&s_attempt, 0, sizeof(s_attempt));
    strlcpy(s_attempt.ssid, ssid, sizeof(s_attempt.ssid));
    if (s_events == NULL) {
        s_attempt.state = NET_ATTEMPT_FAILED;
        s_attempt.reason = NET_REASON_UNAVAILABLE;
        net_attempt_t copy = s_attempt;
        portEXIT_CRITICAL(&s_info_mux);
        net_portal_attempt_done(&copy);
        return ESP_ERR_INVALID_STATE;
    }
    s_attempt.state = NET_ATTEMPT_RUNNING;
    strlcpy(s_req.ssid, ssid, sizeof(s_req.ssid));
    strlcpy(s_req.password, password ? password : "", sizeof(s_req.password));
    s_req_pending = true;
    portEXIT_CRITICAL(&s_info_mux);
    xEventGroupSetBits(s_events, BIT_REQUEST);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void net_wifi_get_attempt(net_attempt_t *out)
{
    portENTER_CRITICAL(&s_info_mux);
    *out = s_attempt;
    portEXIT_CRITICAL(&s_info_mux);
}

void net_wifi_clear_attempt(void)
{
    portENTER_CRITICAL(&s_info_mux);
    if (s_attempt.state != NET_ATTEMPT_RUNNING) {
        memset(&s_attempt, 0, sizeof(s_attempt));
    }
    portEXIT_CRITICAL(&s_info_mux);
}

esp_err_t net_wifi_scan(uint8_t channel, net_ap_info_t *out, size_t max, size_t *n, TickType_t wait)
{
    *n = 0;
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t num = max < SCAN_MAX ? (uint16_t)max : SCAN_MAX;
    wifi_ap_record_t *rec = heap_caps_malloc(num * sizeof(*rec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rec == NULL) {
        rec = malloc(num * sizeof(*rec));
    }
    if (rec == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!net_wifi_radio_lock(wait)) {
        free(rec);
        return ESP_ERR_TIMEOUT;
    }
    const wifi_scan_config_t sc = {
        .channel = channel,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 50, .max = 120},
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&num, rec);
    }
    esp_wifi_clear_ap_list();
    net_wifi_radio_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        free(rec);
        return err;
    }
    for (uint16_t i = 0; i < num; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].ssid, rec[i].ssid, strnlen((const char *)rec[i].ssid, NET_SSID_MAX));
        out[i].rssi = rec[i].rssi;
        out[i].channel = rec[i].primary;
        out[i].secure = rec[i].authmode != WIFI_AUTH_OPEN;
    }
    free(rec);
    *n = net_scan_normalize(out, num);
    return ESP_OK;
}

const char *net_wifi_state_name(net_wifi_state_t state)
{
    switch (state) {
    case NET_WIFI_NO_NETWORKS:
        return "no network configured";
    case NET_WIFI_CONNECTING:
        return "connecting";
    case NET_WIFI_CONNECTED:
        return "connected";
    case NET_WIFI_WAITING:
        return "no configured network reachable, retrying";
    case NET_WIFI_STOPPED:
    default:
        return "off";
    }
}
