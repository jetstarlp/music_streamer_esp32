/*
 * WiFi setup portal: open access point, captive DNS, German web page.
 * Behaviour and rules: see include/net_portal.h.
 *
 * Tasks while the portal runs (both internal RAM, created on start and
 * deleted on stop): "net_portal" (DNS responder + housekeeping) and the
 * esp_http_server task. Page buffers and the scan list live in PSRAM.
 */
#include "net_portal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net_internal.h"
#include "net_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "net_portal";

#define PORTAL_STACK            3584
#define PORTAL_PRIO             3
#define HTTPD_STACK             4096
#define PAGE_BUF_SIZE           (10 * 1024)
#define BODY_MAX                512
#define SCAN_LIST_MAX           20
#define AP_MAX_CLIENTS          4
#define SCAN_WAIT               pdMS_TO_TICKS(25000)    /* a requested attempt may hold the radio */

#define RESTART_AFTER_SUCCESS_US (8LL * 1000000)
#define STOP_AFTER_SUCCESS_US   (30LL * 1000000)
#define STOP_UNUSED_US          (15LL * 60 * 1000000)
#define RESCAN_RETRY_US         (10LL * 1000000)

#if CONFIG_MS_NET_QEMU_OPENETH
#define PORTAL_HAS_WIFI 0       /* test build: page + DNS on the emulated Ethernet */
#else
#define PORTAL_HAS_WIFI 1
#endif

#if CONFIG_MS_NET_PORTAL_SSID_MAC_SUFFIX
#define SSID_MAC_SUFFIX true
#else
#define SSID_MAC_SUFFIX false
#endif

#if CONFIG_MS_NET_PORTAL_RESTART_AFTER_SETUP
#define RESTART_AFTER_SETUP true
#else
#define RESTART_AFTER_SETUP false
#endif

static const uint8_t PORTAL_IP[4] = {192, 168, 4, 1};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_active;          /* task exists                           */
static volatile bool s_stop_req;
static volatile bool s_manual;          /* started by request, not automatically */
static volatile bool s_submitted;       /* a network was entered on the page     */
static volatile int  s_clients;
static volatile uint8_t s_channel;      /* hotspot channel while active          */
static int64_t s_last_client_us;

static net_portal_event_cb_t s_cb;
static void *s_cb_ctx;

static SemaphoreHandle_t s_list_lock;   /* guards s_aps / s_n_aps / s_scanned    */
static net_ap_info_t *s_aps;
static size_t s_n_aps;
static bool s_scanned;

static httpd_handle_t s_httpd;
#if PORTAL_HAS_WIFI
static esp_netif_t *s_ap_netif;
static bool s_events_registered;
#endif

/* ---- small helpers ----------------------------------------------------------- */

void net_portal_set_event_cb(net_portal_event_cb_t cb, void *ctx)
{
    s_cb_ctx = ctx;
    s_cb = cb;
}

static void emit(net_portal_event_id_t id, const char *ssid, uint8_t reason, uint32_t ip)
{
    net_portal_event_cb_t cb = s_cb;
    if (cb == NULL) {
        return;
    }
    net_portal_event_t ev = {
        .id = id,
        .requested = s_manual,
        .clients = s_clients,
        .reason = reason,
        .ip = ip,
    };
    if (ssid) {
        strlcpy(ev.ssid, ssid, sizeof(ev.ssid));
    }
    cb(&ev, s_cb_ctx);
}

void net_portal_get_ssid(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    if (cap == 0) {
        return;
    }
    if (SSID_MAC_SUFFIX && esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    if (net_portal_ssid(CONFIG_MS_NET_PORTAL_SSID, mac, SSID_MAC_SUFFIX, out, cap) == 0) {
        net_portal_ssid("Music-Streamer", mac, SSID_MAC_SUFFIX, out, cap);
    }
}

size_t net_portal_get_qr_payload(char *out, size_t cap)
{
    char ssid[33];
    net_portal_get_ssid(ssid, sizeof(ssid));
    return net_wifi_qr_payload(ssid, NULL, out, cap);
}

const char *net_portal_get_url(void)
{
    return NET_PORTAL_URL;
}

bool net_portal_is_active(void)
{
    return s_active;
}

int net_portal_client_count(void)
{
    return s_clients;
}

bool net_portal_wants_quiet(void)
{
    return s_active && s_clients > 0 && s_channel != 0;
}

uint8_t net_portal_channel(void)
{
    return s_active ? s_channel : 0;
}

void net_portal_attempt_done(const net_attempt_t *at)
{
    if (at->state == NET_ATTEMPT_OK) {
        esp_ip4_addr_t a = {.addr = at->ip};
        ESP_LOGI(TAG, "\"%s\" connected, IP " IPSTR, at->ssid, IP2STR(&a));
        emit(NET_PORTAL_EVT_CONNECT_OK, at->ssid, 0, at->ip);
    } else if (at->state == NET_ATTEMPT_FAILED) {
        ESP_LOGW(TAG, "\"%s\" failed (reason %u)", at->ssid, at->reason);
        emit(NET_PORTAL_EVT_CONNECT_FAILED, at->ssid, at->reason, 0);
    }
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* Refresh the network list shown on the page. */
static void do_scan(void)
{
    net_ap_info_t *tmp = psram_alloc(sizeof(net_ap_info_t) * SCAN_LIST_MAX);
    if (tmp == NULL) {
        return;
    }
    size_t n = 0;
    esp_err_t err = net_wifi_scan(0, tmp, SCAN_LIST_MAX, &n, SCAN_WAIT);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {    /* INVALID_STATE: QEMU, no WiFi */
        xSemaphoreTake(s_list_lock, portMAX_DELAY);
        if (s_aps) {
            memcpy(s_aps, tmp, sizeof(net_ap_info_t) * n);
            s_n_aps = n;
            s_scanned = true;
        }
        xSemaphoreGive(s_list_lock);
        ESP_LOGI(TAG, "%u network(s) in range", (unsigned)n);
    }
    free(tmp);
}

/* ---- HTTP -------------------------------------------------------------------- */

/* Only clients of the hotspot may use the page, not the home network. */
static bool from_hotspot(httpd_req_t *req)
{
#if PORTAL_HAS_WIFI
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    int fd = httpd_req_to_sockfd(req);
    if (getsockname(fd, (struct sockaddr *)&local, &len) != 0) {
        return false;
    }
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)&local;
        return memcmp(&in->sin_addr.s_addr, PORTAL_IP, 4) == 0;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {     /* IPv4-mapped IPv6 address */
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&local;
        const uint8_t *a = (const uint8_t *)&in6->sin6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return memcmp(a, mapped, 12) == 0 && memcmp(a + 12, PORTAL_IP, 4) == 0;
    }
#endif
    return false;
#else
    (void)req;
    return true;
#endif
}

static esp_err_t send_redirect(httpd_req_t *req, const char *status, const char *location)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    /* iOS only shows the portal if the redirect has a body */
    return httpd_resp_sendstr(req, "<a href=\"" NET_PORTAL_URL "\">Music Streamer einrichten</a>");
}

static esp_err_t refuse(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "Nur ueber den Einrichtungs-Hotspot erreichbar.");
}

/* Requests for other host names (captive-portal checks, typed addresses) are
 * sent to the portal's own address so that the browser shows 192.168.4.1. */
static bool foreign_host(httpd_req_t *req)
{
    char host[48];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;   /* missing or absurdly long: serve the page anyway */
    }
    return strcmp(host, NET_PORTAL_IP) != 0 && strcmp(host, NET_PORTAL_IP ":80") != 0;
}

static esp_err_t send_page(httpd_req_t *req, net_page_t *pg)
{
    char ap_ssid[33];
    net_portal_get_ssid(ap_ssid, sizeof(ap_ssid));
    pg->ap_ssid = ap_ssid;
    pg->device = net_wifi_device_name();

    char *mem = psram_alloc(PAGE_BUF_SIZE);
    if (mem == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    net_buf_t b;
    net_buf_init(&b, mem, PAGE_BUF_SIZE);
    net_ap_info_t *aps = NULL;
    if (pg->kind == NET_PAGE_FORM) {
        aps = psram_alloc(sizeof(net_ap_info_t) * SCAN_LIST_MAX);
        if (aps) {
            xSemaphoreTake(s_list_lock, portMAX_DELAY);
            if (s_aps) {
                memcpy(aps, s_aps, sizeof(net_ap_info_t) * s_n_aps);
                pg->n_aps = s_n_aps;
            }
            pg->scanning = !s_scanned;
            xSemaphoreGive(s_list_lock);
            pg->aps = aps;
        }
    }
    net_page_render(&b, pg);
    free(aps);
    if (b.overflow) {
        ESP_LOGW(TAG, "page truncated");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, mem, (ssize_t)b.len);
    free(mem);
    return err;
}

/* The page that belongs to the current state of the entered network. */
static esp_err_t send_state_page(httpd_req_t *req)
{
    net_attempt_t at;
    net_wifi_get_attempt(&at);
    net_page_t pg = {.kind = NET_PAGE_FORM, .ssid = at.ssid};
    char ip[16];
    switch (at.state) {
    case NET_ATTEMPT_RUNNING:
        pg.kind = NET_PAGE_CONNECTING;
        break;
    case NET_ATTEMPT_OK: {
        esp_ip4_addr_t a = {.addr = at.ip};
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        pg.kind = NET_PAGE_CONNECTED;
        pg.ip = ip;
        pg.restarting = RESTART_AFTER_SETUP && !net_ota_pending_verify();
        break;
    }
    case NET_ATTEMPT_FAILED:
        pg.error = net_wifi_reason_text_de(at.reason);
        break;
    case NET_ATTEMPT_NONE:
    default:
        pg.ssid = NULL;
        break;
    }
    return send_page(req, &pg);
}

static esp_err_t h_root(httpd_req_t *req)
{
    if (!from_hotspot(req)) {
        return refuse(req);
    }
    if (foreign_host(req)) {
        return send_redirect(req, "302 Found", NET_PORTAL_URL);
    }
    /* The phone may have dropped off the hotspot while the device connected
     * and come back through the captive-portal check: show the result. */
    return send_state_page(req);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    if (!from_hotspot(req)) {
        return refuse(req);
    }
    net_wifi_clear_attempt();
    do_scan();
    return send_redirect(req, "303 See Other", "/");
}

static esp_err_t h_connect(httpd_req_t *req)
{
    if (!from_hotspot(req)) {
        return refuse(req);
    }
    char *body = psram_alloc(BODY_MAX + 1);
    char *ssid = psram_alloc(NET_SSID_MAX + 2);
    char *pass = psram_alloc(66);
    esp_err_t ret = ESP_OK;
    if (!body || !ssid || !pass) {
        ret = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        goto out;
    }
    size_t got = 0;
    if (req->content_len > BODY_MAX) {
        ret = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
        goto out;
    }
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ret = ESP_FAIL;
            goto out;
        }
        got += (size_t)r;
    }
    body[got] = '\0';

    const char *err = NULL;
    if (!net_form_get(body, got, "s", ssid, NET_SSID_MAX + 2)) {
        ssid[0] = '\0';
        err = "Der WLAN-Name ist zu lang oder fehlt.";
    }
    if (!err && !net_form_get(body, got, "p", pass, 66)) {
        pass[0] = '\0';
        err = "Das Passwort ist zu lang (höchstens 64 Zeichen).";
    }
    if (!err) {
        err = net_validate_credentials(ssid, pass);
    }
    if (err) {
        net_page_t pg = {.kind = NET_PAGE_FORM, .error = err, .ssid = ssid};
        ret = send_page(req, &pg);
        goto out;
    }
    ESP_LOGI(TAG, "network \"%s\" entered on the setup page", ssid);
    s_submitted = true;
    emit(NET_PORTAL_EVT_CREDENTIALS, ssid, 0, 0);
    net_wifi_request(ssid, pass);       /* the result is shown by /status */
    ret = send_redirect(req, "303 See Other", "/status");
out:
    if (body) {
        memset(body, 0, BODY_MAX + 1);
    }
    if (pass) {
        memset(pass, 0, 66);
    }
    free(body);
    free(ssid);
    free(pass);
    return ret;
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!from_hotspot(req)) {
        return refuse(req);
    }
    return send_state_page(req);
}

/* Everything else (generate_204, hotspot-detect.html, connecttest.txt, ...):
 * redirect to the portal. That makes the phone show its "sign in to network"
 * notification, which opens the form. */
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    if (!from_hotspot(req)) {
        return refuse(req);
    }
    return send_redirect(req, "302 Found", NET_PORTAL_URL);
}

static esp_err_t http_up(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = HTTPD_STACK;
    cfg.core_id = 0;
    cfg.task_priority = PORTAL_PRIO;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd_start");
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/connect", .method = HTTP_POST, .handler = h_connect},
        {.uri = "/status", .method = HTTP_GET, .handler = h_status},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_not_found);
    /* Captive-portal checks produce a steady stream of odd requests. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    return ESP_OK;
}

/* ---- access point ----------------------------------------------------------------- */

#if PORTAL_HAS_WIFI
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (!s_active) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_clients > 0) {
        s_clients--;
    }
    portEXIT_CRITICAL(&s_mux);
    s_last_client_us = esp_timer_get_time();
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a phone joined the hotspot (%d)", s_clients);
        emit(NET_PORTAL_EVT_CLIENT_JOINED, NULL, 0, 0);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "a phone left the hotspot (%d)", s_clients);
        emit(NET_PORTAL_EVT_CLIENT_LEFT, NULL, 0, 0);
        net_wifi_rescan();      /* background attempts may use all channels again */
    }
}

/* The hotspot's channel: the station's if it is connected (the radio has only
 * one), else the channel of the strongest stored or, failing that, the
 * strongest network in range (see net_pick_ap_channel). */
static uint8_t pick_channel(void)
{
    wifi_ap_record_t sta;
    if (esp_wifi_sta_get_ap_info(&sta) == ESP_OK && sta.primary >= 1 && sta.primary <= 13) {
        return sta.primary;
    }
    char (*known)[33] = psram_alloc(SETTINGS_WIFI_MAX * 33);
    const char *ptrs[SETTINGS_WIFI_MAX];
    size_t n_known = 0;
    if (known) {
        n_known = net_wifi_known_ssids(known, SETTINGS_WIFI_MAX);
        for (size_t i = 0; i < n_known; i++) {
            ptrs[i] = known[i];
        }
    }
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    uint8_t ch = net_pick_ap_channel(s_aps, s_n_aps, ptrs, n_known);
    xSemaphoreGive(s_list_lock);
    free(known);
    return ch;
}

static esp_err_t ap_up(void)
{
    if (!s_events_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                                on_ap_event, NULL, NULL), TAG, "event");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                                on_ap_event, NULL, NULL), TAG, "event");
        s_events_registered = true;
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();    /* 192.168.4.1/24 with DHCP server */
        ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "ap netif");
        /* DHCP option 114 (RFC 8910): newer Android/iOS open the portal from
         * it. The DNS option is the hotspot itself (CONFIG_LWIP_DHCPS_ADD_DNS). */
        static const char uri[] = NET_PORTAL_URL;
        esp_netif_dhcps_stop(s_ap_netif);
        esp_err_t e = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                             (void *)uri, sizeof(uri) - 1);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "DHCP captive-portal option: %s", esp_err_to_name(e));
        }
        esp_netif_dhcps_start(s_ap_netif);
    }

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    char ssid[33];
    net_portal_get_ssid(ssid, sizeof(ssid));
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = AP_MAX_CLIENTS;
    ap.ap.beacon_interval = 100;
    ap.ap.channel = pick_channel();

    /* Mode changes must not cut into a connection attempt. */
    if (!net_wifi_radio_lock(SCAN_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    net_wifi_radio_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "AP mode");
    s_channel = ap.ap.channel;
    ESP_LOGI(TAG, "setup hotspot \"%s\" (open) on channel %u, page " NET_PORTAL_URL, ssid, ap.ap.channel);
    return ESP_OK;
}

static void ap_down(void)
{
    bool locked = net_wifi_radio_lock(SCAN_WAIT);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (locked) {
        net_wifi_radio_unlock();
    }
    portENTER_CRITICAL(&s_mux);
    s_clients = 0;
    s_channel = 0;
    portEXIT_CRITICAL(&s_mux);
}

/* Current hotspot channel (follows the station when it connects). */
static void update_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0) {
        s_channel = primary;
    }
}
#else
static esp_err_t ap_up(void)
{
    ESP_LOGW(TAG, "QEMU test build: setup page and DNS on the emulated Ethernet, no hotspot");
    return ESP_OK;
}

static void ap_down(void)
{
}

static void update_channel(void)
{
}
#endif

/* ---- DNS + housekeeping task ------------------------------------------------------- */

static int dns_up(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "dns socket: errno %d", errno);
        return -1;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53)};
#if PORTAL_HAS_WIFI
    memcpy(&addr.sin_addr.s_addr, PORTAL_IP, 4);    /* not on the home network */
#else
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns bind: errno %d", errno);
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

typedef enum {
    KEEP_RUNNING,
    STOP,
    STOP_AND_RESTART,
} verdict_t;

static verdict_t should_stop(int64_t now)
{
    static int64_t success_us;
    if (s_stop_req) {
        success_us = 0;
        ESP_LOGI(TAG, "stop requested");
        return STOP;
    }
    net_attempt_t at;
    net_wifi_get_attempt(&at);
    if (s_submitted && at.state == NET_ATTEMPT_OK) {
        if (success_us == 0) {
            success_us = now;
        }
        const bool restart = RESTART_AFTER_SETUP && !net_ota_pending_verify();
        if (now - success_us > (restart ? RESTART_AFTER_SUCCESS_US : STOP_AFTER_SUCCESS_US)) {
            success_us = 0;
            ESP_LOGI(TAG, "network \"%s\" set up", at.ssid);
            return restart ? STOP_AND_RESTART : STOP;
        }
        return KEEP_RUNNING;
    }
    success_us = 0;
    if (!s_manual && net_wifi_is_connected() && s_clients == 0 && at.state != NET_ATTEMPT_RUNNING) {
        ESP_LOGI(TAG, "WiFi connected again");
        return STOP;
    }
    if (s_manual && s_clients == 0 && now - s_last_client_us > STOP_UNUSED_US) {
        ESP_LOGI(TAG, "nobody used the hotspot for 15 min");
        return STOP;
    }
    return KEEP_RUNNING;
}

static void portal_task(void *arg)
{
    (void)arg;
    int dns = -1;
    uint8_t *q = psram_alloc(512);
    uint8_t *r = psram_alloc(600);
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    s_aps = psram_alloc(sizeof(net_ap_info_t) * SCAN_LIST_MAX);
    s_n_aps = 0;
    s_scanned = false;
    xSemaphoreGive(s_list_lock);
    net_wifi_clear_attempt();

    esp_err_t err = (q && r && s_aps) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        do_scan();      /* before the hotspot is up: no phone to disturb, and it picks the channel */
        err = ap_up();
    }
    if (err == ESP_OK) {
        err = http_up();
    }
    if (err == ESP_OK) {
        dns = dns_up();
        err = dns < 0 ? ESP_FAIL : ESP_OK;
    }
    char ap_ssid[33];
    net_portal_get_ssid(ap_ssid, sizeof(ap_ssid));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup hotspot not available: %s", esp_err_to_name(err));
    } else {
        emit(NET_PORTAL_EVT_STARTED, ap_ssid, 0, 0);
    }

    verdict_t verdict = STOP;
    unsigned dns_answers = 0;
    int64_t last_tick = 0;
    int64_t last_scan_try = esp_timer_get_time();
    while (err == ESP_OK) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(dns, q, 512, 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            size_t rl = net_dns_build_reply(q, (size_t)n, PORTAL_IP, r, 600);
            if (rl > 0 && sendto(dns, r, rl, 0, (struct sockaddr *)&from, from_len) == (int)rl) {
                dns_answers++;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_tick < 1000000) {
            continue;
        }
        last_tick = now;
        update_channel();
        if (!s_scanned && now - last_scan_try > RESCAN_RETRY_US) {
            last_scan_try = now;
            do_scan();
        }
        verdict = should_stop(now);
        if (verdict != KEEP_RUNNING) {
            break;
        }
    }

    if (verdict == STOP_AND_RESTART) {
        emit(NET_PORTAL_EVT_RESTARTING, NULL, 0, 0);
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (dns >= 0) {
        close(dns);
    }
    ap_down();
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    free(s_aps);
    s_aps = NULL;
    s_n_aps = 0;
    xSemaphoreGive(s_list_lock);
    free(q);
    free(r);
    if (verdict == STOP_AND_RESTART) {
        ESP_LOGW(TAG, "restarting with the new WiFi network");
        vTaskDelay(pdMS_TO_TICKS(500));     /* time for the application to mute */
        esp_restart();
    }
    ESP_LOGI(TAG, "setup hotspot stopped (%u DNS answers sent)", dns_answers);
    portENTER_CRITICAL(&s_mux);
    s_active = false;
    s_stop_req = false;
    portEXIT_CRITICAL(&s_mux);
    if (err == ESP_OK) {
        emit(NET_PORTAL_EVT_STOPPED, ap_ssid, 0, 0);
    }
    net_wifi_rescan();
    vTaskDelete(NULL);
}

static esp_err_t portal_start(bool manual)
{
#if PORTAL_HAS_WIFI
    if (!net_wifi_started()) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (s_list_lock == NULL) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(lock, ESP_ERR_NO_MEM, TAG, "mutex");
        portENTER_CRITICAL(&s_mux);
        if (s_list_lock == NULL) {
            s_list_lock = lock;
            lock = NULL;
        }
        portEXIT_CRITICAL(&s_mux);
        if (lock) {
            vSemaphoreDelete(lock);
        }
    }
    portENTER_CRITICAL(&s_mux);
    if (s_active) {
        if (manual) {
            s_manual = true;
        }
        s_stop_req = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_OK;
    }
    s_active = true;
    s_stop_req = false;
    s_manual = manual;
    s_submitted = false;
    s_clients = 0;
    s_channel = 0;
    portEXIT_CRITICAL(&s_mux);
    s_last_client_us = esp_timer_get_time();

    if (xTaskCreatePinnedToCore(portal_task, "net_portal", PORTAL_STACK, NULL, PORTAL_PRIO, NULL, 0) != pdPASS) {
        s_active = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "starting the setup hotspot (%s)", manual ? "requested" : "automatic");
    return ESP_OK;
}

esp_err_t net_portal_start(void)
{
    return portal_start(true);
}

void net_portal_auto_start(void)
{
    portal_start(false);
}

void net_portal_stop(void)
{
    if (s_active) {
        s_stop_req = true;
    }
}
