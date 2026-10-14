/*
 * Host unit tests for the pure parts of components/net (src/pure).
 * Run: firmware/tools/host_test_net.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_pure.h"

static int s_fail;
static int s_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                            \
        }                                                                        \
    } while (0)

static int cmp(const char *a, const char *b)
{
    int c = 99;
    if (!net_semver_compare(a, b, &c)) {
        return 99;
    }
    return c;
}

static void test_semver(void)
{
    CHECK(cmp("1.2.3", "1.2.3") == 0);
    CHECK(cmp("1.2.3", "1.2.4") == -1);
    CHECK(cmp("1.10.0", "1.9.9") == 1);
    CHECK(cmp("2.0.0", "10.0.0") == -1);
    CHECK(cmp("v1.2.0", "1.2.0") == 0);
    CHECK(cmp("1.2", "1.2.0") == 0);
    CHECK(cmp("1", "1.0.0") == 0);
    CHECK(cmp("1.0.0+build.7", "1.0.0") == 0);
    /* semver.org precedence example */
    const char *order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); i++) {
        CHECK(cmp(order[i], order[i + 1]) == -1);
        CHECK(cmp(order[i + 1], order[i]) == 1);
    }
    CHECK(cmp("1.0.0-alpha.01", "1.0.0-alpha.1") == 0);
    CHECK(cmp("4294967295.0.0", "1.0.0") == 1);
    /* invalid */
    CHECK(cmp("", "1.0.0") == 99);
    CHECK(cmp("1.2.3.4", "1.0.0") == 99);
    CHECK(cmp("4294967296.0.0", "1.0.0") == 99);
    CHECK(cmp("1.2.x", "1.0.0") == 99);
    CHECK(cmp("1.2.3-", "1.0.0") == 99);
    CHECK(cmp("1.2.3-a..b", "1.0.0") == 99);
    CHECK(cmp("1.2.3+", "1.0.0") == 99);
    CHECK(cmp("93a8503", "1.0.0") == 99);
    CHECK(cmp(NULL, "1.0.0") == 99);
    CHECK(!net_semver_valid("1.0.0 "));
    CHECK(net_semver_valid("1.0.0-rc.1+sha.5114f85"));

}

static void test_decide(void)
{
    CHECK(net_ota_decide("1.0.0", "1.0.1", "") == NET_OTA_DECIDE_INSTALL);
    CHECK(net_ota_decide("1.0.0", "1.0.0", "") == NET_OTA_DECIDE_UP_TO_DATE);
    CHECK(net_ota_decide("1.1.0", "1.0.9", "") == NET_OTA_DECIDE_UP_TO_DATE);
    CHECK(net_ota_decide("1.1.0-rc.1", "1.1.0", NULL) == NET_OTA_DECIDE_INSTALL);
    CHECK(net_ota_decide("1.1.0", "1.1.0-rc.1", NULL) == NET_OTA_DECIDE_UP_TO_DATE);
    CHECK(net_ota_decide("1.0.0", "1.0.0+build.2", NULL) == NET_OTA_DECIDE_UP_TO_DATE);
    CHECK(net_ota_decide("1.0.0", "2.0.0", "1.5.0") == NET_OTA_DECIDE_TOO_OLD);
    CHECK(net_ota_decide("1.5.0", "2.0.0", "1.5.0") == NET_OTA_DECIDE_INSTALL);
    /* old developer builds were named after git describe: count as 0.0.0 */
    CHECK(net_ota_decide("93a8503-dirty", "2.0.0", "1.5.0") == NET_OTA_DECIDE_INSTALL);
    CHECK(net_ota_decide("93a8503", "0.0.0", "") == NET_OTA_DECIDE_UP_TO_DATE);
    CHECK(net_ota_decide(NULL, "1.0.0", "") == NET_OTA_DECIDE_INSTALL);
    CHECK(net_ota_decide("1.0.0", "latest", "") == NET_OTA_DECIDE_INVALID);
    CHECK(net_ota_decide("1.0.0", NULL, "") == NET_OTA_DECIDE_INVALID);
    CHECK(net_ota_decide("1.0.0", "1.2.0", "x") == NET_OTA_DECIDE_INVALID);
}

static void test_hex_url(void)
{
    uint8_t b[4];
    CHECK(net_hex_decode("00fFa5", b, 3) && b[0] == 0 && b[1] == 0xff && b[2] == 0xa5);
    CHECK(!net_hex_decode("00fFa5", b, 2));           /* too long */
    CHECK(!net_hex_decode("00fF", b, 3));             /* too short */
    CHECK(!net_hex_decode("0g", b, 1));

    CHECK(net_url_valid("https://user.github.io/ota/manifest.json", false, 256));
    CHECK(!net_url_valid("http://user.github.io/x", false, 256));
    CHECK(net_url_valid("http://10.0.2.2:8070/x", true, 256));
    CHECK(!net_url_valid("ftp://x/y", true, 256));
    CHECK(!net_url_valid("https://", false, 256));
    CHECK(!net_url_valid("https:///path", false, 256));
    CHECK(!net_url_valid("https://a b/c", false, 256));
    CHECK(!net_url_valid("https://ab/c", false, 12));
    CHECK(!net_url_valid(NULL, false, 256));

    char out[128];
    const char *base = "https://u.github.io/repo/ota/manifest.json?x=1";
    CHECK(net_url_resolve(base, "ms-1.2.0.bin", out, sizeof(out)) &&
          strcmp(out, "https://u.github.io/repo/ota/ms-1.2.0.bin") == 0);
    CHECK(net_url_resolve(base, "/a/b.bin", out, sizeof(out)) && strcmp(out, "https://u.github.io/a/b.bin") == 0);
    CHECK(net_url_resolve(base, "//cdn.example.com/b.bin", out, sizeof(out)) &&
          strcmp(out, "https://cdn.example.com/b.bin") == 0);
    CHECK(net_url_resolve(base, "https://x.y/z.bin", out, sizeof(out)) && strcmp(out, "https://x.y/z.bin") == 0);
    CHECK(net_url_resolve("https://github.com/u/r/releases/latest/download/manifest.json", "app.bin", out,
                          sizeof(out)) &&
          strcmp(out, "https://github.com/u/r/releases/latest/download/app.bin") == 0);
    CHECK(net_url_resolve("https://host", "a.bin", out, sizeof(out)) && strcmp(out, "https://host/a.bin") == 0);
    CHECK(net_url_resolve("http://10.0.2.2:8070/m.json", "a.bin", out, sizeof(out)) &&
          strcmp(out, "http://10.0.2.2:8070/a.bin") == 0);
    CHECK(!net_url_resolve("ftp://h/m.json", "a.bin", out, sizeof(out)));
    CHECK(!net_url_resolve(base, "", out, sizeof(out)));
    CHECK(!net_url_resolve(base, "a-name-that-is-too-long-for-the-buffer.bin", out, 30));
}

static void test_text(void)
{
    char out[64];
    CHECK(net_html_escape("<a href=\"x\">&'", out, sizeof(out)) &&
          strcmp(out, "&lt;a href=&quot;x&quot;&gt;&amp;&#39;") == 0);
    CHECK(net_html_escape("a\tb", out, sizeof(out)) && strcmp(out, "a?b") == 0);
    CHECK(!net_html_escape("&&&", out, 10) && strcmp(out, "&amp;") == 0);   /* never half an entity */
    CHECK(net_html_escape(NULL, out, sizeof(out)) && out[0] == 0);

    const char body[] = "s=My+Home%21&p=p%40ss+w%C3%B6rd&empty=&flag";
    size_t n = strlen(body);
    CHECK(net_form_get(body, n, "s", out, sizeof(out)) && strcmp(out, "My Home!") == 0);
    CHECK(net_form_get(body, n, "p", out, sizeof(out)) && strcmp(out, "p@ss w\xc3\xb6rd") == 0);
    CHECK(net_form_get(body, n, "empty", out, sizeof(out)) && out[0] == 0);
    CHECK(net_form_get(body, n, "flag", out, sizeof(out)) && out[0] == 0);
    CHECK(!net_form_get(body, n, "x", out, sizeof(out)));
    CHECK(!net_form_get(body, n, "s", out, 5));                   /* too long */
    CHECK(!net_form_get("s=a%00b", 7, "s", out, sizeof(out)));     /* NUL */
    CHECK(!net_form_get("s=a%4", 5, "s", out, sizeof(out)));       /* truncated escape */
    CHECK(!net_form_get("s=a%zz", 6, "s", out, sizeof(out)));
    CHECK(net_form_get("ss=1&s=2", 8, "s", out, sizeof(out)) && strcmp(out, "2") == 0);
    /* body not NUL-terminated: only `len` bytes count */
    CHECK(net_form_get("s=abcdef", 4, "s", out, sizeof(out)) && strcmp(out, "ab") == 0);

    CHECK(net_validate_credentials("Home", "") == NULL);
    CHECK(net_validate_credentials("Home", NULL) == NULL);
    CHECK(net_validate_credentials("Home", "12345678") == NULL);
    CHECK(net_validate_credentials("Home", "1234567") != NULL);
    CHECK(net_validate_credentials("", "12345678") != NULL);
    CHECK(net_validate_credentials("123456789012345678901234567890123", "") != NULL);   /* 33 */
    CHECK(net_validate_credentials("12345678901234567890123456789012", "") == NULL);    /* 32 */
    CHECK(net_validate_credentials("Home", "pass\xc3\xb6word") != NULL);   /* umlaut */
    char hex64[65];
    memset(hex64, 'a', 64);
    hex64[64] = 0;
    CHECK(net_validate_credentials("Home", hex64) == NULL);
    hex64[10] = 'x';
    CHECK(net_validate_credentials("Home", hex64) != NULL);
}

static void test_qr(void)
{
    char out[96];
    CHECK(net_wifi_qr_payload("Music-Streamer-AB12", NULL, out, sizeof(out)) > 0 &&
          strcmp(out, "WIFI:T:nopass;S:Music-Streamer-AB12;;") == 0);
    CHECK(net_wifi_qr_payload("a;b", "p:w\\", out, sizeof(out)) > 0 &&
          strcmp(out, "WIFI:T:WPA;S:a\\;b;P:p\\:w\\\\;;") == 0);
    CHECK(net_wifi_qr_payload("", NULL, out, sizeof(out)) == 0);
    CHECK(net_wifi_qr_payload("Music-Streamer-AB12", NULL, out, 20) == 0 && out[0] == 0);
    size_t need = strlen("WIFI:T:nopass;S:X;;");
    CHECK(net_wifi_qr_payload("X", "", out, need) == 0);
    CHECK(net_wifi_qr_payload("X", "", out, need + 1) == need);
}

static void test_scan(void)
{
    net_ap_info_t aps[] = {
        {"Home", -70, 1, true},  {"", -30, 6, true},       {"Cafe", -60, 11, false},
        {"Home", -50, 36, true}, {"Neighbour", -60, 6, true}, {"Weak", -90, 1, true},
    };
    size_t n = net_scan_normalize(aps, sizeof(aps) / sizeof(aps[0]));
    CHECK(n == 4);
    CHECK(strcmp(aps[0].ssid, "Home") == 0 && aps[0].rssi == -50 && aps[0].channel == 36);
    CHECK(strcmp(aps[1].ssid, "Cafe") == 0);          /* stable for equal RSSI */
    CHECK(strcmp(aps[2].ssid, "Neighbour") == 0);
    CHECK(strcmp(aps[3].ssid, "Weak") == 0);
    CHECK(net_scan_normalize(NULL, 3) == 0);
    CHECK(net_rssi_bars(-40) == 4 && net_rssi_bars(-60) == 3 && net_rssi_bars(-75) == 2 && net_rssi_bars(-95) == 1);
    /* lookup, hotspot channel, hotspot name */
    net_ap_info_t ch[] = {{"Nachbar", -40, 11, true}, {"Zuhause", -70, 6, true}, {"Weit", -90, 14, true}};
    CHECK(net_scan_find(ch, 3, "Zuhause") == 1 && net_scan_find(ch, 3, "zuhause") == -1);
    CHECK(net_scan_find(ch, 3, "") == -1 && net_scan_find(NULL, 0, "x") == -1);
    const char *known[] = {"Alt", "Zuhause"};
    CHECK(net_pick_ap_channel(ch, 3, known, 2) == 6);       /* stored network wins */
    CHECK(net_pick_ap_channel(ch, 3, NULL, 0) == 11);       /* else the strongest */
    CHECK(net_pick_ap_channel(ch + 2, 1, NULL, 0) == 1);    /* channel 14: not used */
    CHECK(net_pick_ap_channel(NULL, 0, known, 2) == 1);
    char name[40];
    const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0x01, 0xab, 0x0c};
    CHECK(net_portal_ssid("Music-Streamer", mac, false, name, sizeof(name)) == 14 &&
          strcmp(name, "Music-Streamer") == 0);
    CHECK(net_portal_ssid("Music-Streamer", mac, true, name, sizeof(name)) == 19 &&
          strcmp(name, "Music-Streamer-AB0C") == 0);
    CHECK(net_portal_ssid("0123456789012345678901234567890123456789", mac, true, name, sizeof(name)) == 32 &&
          strcmp(name, "012345678901234567890123456-AB0C") == 0);
    CHECK(net_portal_ssid("01234567890123456789012345\xc3\xa4xyz", mac, true, name, sizeof(name)) == 31);
    CHECK(net_portal_ssid("", mac, true, name, sizeof(name)) == 0 && name[0] == 0);
    CHECK(net_portal_ssid("Music-Streamer", mac, true, name, 19) == 0);

}

static size_t make_query(uint8_t *q, const char *name, uint16_t qtype, int qdcount)
{
    static const uint8_t hdr[12] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(q, hdr, 12);
    q[5] = (uint8_t)qdcount;
    size_t pos = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        q[pos++] = (uint8_t)l;
        memcpy(q + pos, s, l);
        pos += l;
        s += l + (dot ? 1 : 0);
    }
    q[pos++] = 0;
    q[pos++] = (uint8_t)(qtype >> 8);
    q[pos++] = (uint8_t)qtype;
    q[pos++] = 0;
    q[pos++] = 1;
    return pos;

}

static void test_dns(void)
{
    const uint8_t ip[4] = {192, 168, 4, 1};
    uint8_t q[300], r[320];
    size_t ql = make_query(q, "connectivitycheck.gstatic.com", 1, 1);
    size_t rl = net_dns_build_reply(q, ql, ip, r, sizeof(r));
    CHECK(rl == ql + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34);                   /* same id */
    CHECK((r[2] & 0x80) && (r[2] & 0x04) && (r[2] & 0x01)); /* QR, AA, RD copied */
    CHECK(r[3] == 0x80);                                    /* RA, NOERROR */
    CHECK(r[7] == 1 && r[11] == 0);                         /* one answer, no additional */
    CHECK(memcmp(r + 12, q + 12, ql - 12) == 0);            /* question echoed */
    CHECK(r[ql] == 0xC0 && r[ql + 1] == 12);
    CHECK(memcmp(r + rl - 4, ip, 4) == 0);

    ql = make_query(q, "example.com", 28, 1);               /* AAAA: empty answer */
    rl = net_dns_build_reply(q, ql, ip, r, sizeof(r));
    CHECK(rl == ql && r[7] == 0 && (r[3] & 0x0F) == 0);

    /* with an EDNS OPT record appended (ARCOUNT 1): dropped */
    ql = make_query(q, "a.b", 1, 1);
    q[11] = 1;
    static const uint8_t opt[11] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(q + ql, opt, sizeof(opt));
    rl = net_dns_build_reply(q, ql + sizeof(opt), ip, r, sizeof(r));
    CHECK(rl == ql + 16 && r[11] == 0);

    CHECK(net_dns_build_reply(q, 11, ip, r, sizeof(r)) == 0);          /* short */
    ql = make_query(q, "a.b", 1, 2);
    CHECK(net_dns_build_reply(q, ql, ip, r, sizeof(r)) == 0);          /* 2 questions */
    ql = make_query(q, "a.b", 1, 1);
    q[2] |= 0x80;
    CHECK(net_dns_build_reply(q, ql, ip, r, sizeof(r)) == 0);          /* response */
    ql = make_query(q, "a.b", 1, 1);
    q[2] |= 0x28;
    CHECK(net_dns_build_reply(q, ql, ip, r, sizeof(r)) == 0);          /* opcode 5 */
    ql = make_query(q, "a.b", 1, 1);
    CHECK(net_dns_build_reply(q, ql - 1, ip, r, sizeof(r)) == 0);      /* truncated */
    CHECK(net_dns_build_reply(q, ql, ip, r, ql + 15) == 0);            /* out too small */
    q[12] = 0xC0;
    CHECK(net_dns_build_reply(q, ql, ip, r, sizeof(r)) == 0);          /* compression */
    q[12] = 70;
    CHECK(net_dns_build_reply(q, ql, ip, r, sizeof(r)) == 0);          /* label runs past end */

    /* random garbage must never crash or overrun */
    srand(1);
    for (int i = 0; i < 20000; i++) {
        size_t len = (size_t)(rand() % 64);
        for (size_t k = 0; k < len; k++) {
            q[k] = (uint8_t)rand();
        }
        if (len > 5 && (i & 1)) {
            q[2] = 0;
            q[4] = 0;
            q[5] = 1;
        }
        rl = net_dns_build_reply(q, len, ip, r, (size_t)(rand() % 100));
        CHECK(rl <= 100);
    }
}

#ifndef NET_TEST_NO_CJSON
static void test_manifest(void)
{
    const char *murl = "https://u.github.io/ota/manifest.json";
    char sha[65];
    memset(sha, 'A', 64);
    sha[64] = 0;
    char json[1024];
    net_manifest_t m;

    snprintf(json, sizeof(json),
             "{\"version\":\"1.2.0\",\"url\":\"ms-waveshare-app-1.2.0.bin\",\"sha256\":\"%s\","
             "\"size\":1480000,\"min_version\":\"1.0.0\",\"notes\":\"Neu: WLAN-Portal\",\"extra\":[1,2]}",
             sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) == NULL);
    CHECK(strcmp(m.version, "1.2.0") == 0);
    CHECK(strcmp(m.url, "https://u.github.io/ota/ms-waveshare-app-1.2.0.bin") == 0);
    CHECK(m.sha256[0] == 0xAA && m.sha256[31] == 0xAA);
    CHECK(m.size == 1480000 && strcmp(m.min_version, "1.0.0") == 0);
    CHECK(strcmp(m.notes, "Neu: WLAN-Portal") == 0);

    snprintf(json, sizeof(json), "{\"version\":\"1.2.0\",\"url\":\"https://x.y/a.bin\",\"sha256\":\"%s\"}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) == NULL && m.size == 0 && m.min_version[0] == 0);

    /* errors */
    CHECK(net_manifest_parse("", 0, murl, false, &m) != NULL);
    CHECK(net_manifest_parse("[1]", 3, murl, false, &m) != NULL);
    CHECK(net_manifest_parse("{\"version\":", 11, murl, false, &m) != NULL);
    snprintf(json, sizeof(json), "{\"url\":\"a.bin\",\"sha256\":\"%s\"}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);
    snprintf(json, sizeof(json), "{\"version\":\"latest\",\"url\":\"a.bin\",\"sha256\":\"%s\"}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);
    snprintf(json, sizeof(json), "{\"version\":1,\"url\":\"a.bin\",\"sha256\":\"%s\"}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);
    CHECK(net_manifest_parse("{\"version\":\"1.0.0\",\"url\":\"a.bin\",\"sha256\":\"abc\"}", 48, murl, false, &m) !=
          NULL);
    snprintf(json, sizeof(json), "{\"version\":\"1.2.0\",\"url\":\"http://x.y/a.bin\",\"sha256\":\"%s\"}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);   /* http refused */
    CHECK(net_manifest_parse(json, strlen(json), murl, true, &m) == NULL);    /* test builds */
    snprintf(json, sizeof(json), "{\"version\":\"1.2.0\",\"url\":\"a.bin\",\"sha256\":\"%s\",\"size\":-5}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);
    snprintf(json, sizeof(json), "{\"version\":\"1.2.0\",\"url\":\"a.bin\",\"sha256\":\"%s\",\"size\":1.5}", sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);
    snprintf(json, sizeof(json), "{\"version\":\"1.2.0\",\"url\":\"a.bin\",\"sha256\":\"%s\",\"min_version\":\"x\"}",
             sha);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) != NULL);
    /* a failed parse leaves nothing behind */
    CHECK(m.version[0] == 0 && m.url[0] == 0);

    /* notes are cut at a UTF-8 boundary */
    char notes[400];
    memset(notes, 0, sizeof(notes));
    for (int i = 0; i < 100; i++) {
        strcat(notes, "\xc3\xa4");     /* ä, 2 bytes each */
    }
    snprintf(json, sizeof(json), "{\"version\":\"1.2.0\",\"url\":\"a.bin\",\"sha256\":\"%s\",\"notes\":\"xy%s\"}", sha,
             notes);
    CHECK(net_manifest_parse(json, strlen(json), murl, false, &m) == NULL);
    CHECK(strlen(m.notes) == NET_NOTES_MAX - 2);   /* "xy" + 78 * 2 bytes, not half of the 79th */
}
#endif

static void test_pages(void)
{
    static char mem[8192];
    net_buf_t b;
    net_ap_info_t aps[] = {{"<script>", -50, 1, true}, {"Open \"Cafe\"", -80, 6, false}};
    net_page_t pg = {.kind = NET_PAGE_FORM, .aps = aps, .n_aps = 2, .error = "Falsches Passwort?",
                     .ssid = "a&b", .ap_ssid = "Music-Streamer-AB12", .device = "Wohnzimmer"};
    net_buf_init(&b, mem, sizeof(mem));
    net_page_render(&b, &pg);
    CHECK(!b.overflow);
    CHECK(strstr(mem, "<!DOCTYPE html>") == mem);
    CHECK(strstr(mem, "</html>") != NULL);
    CHECK(strstr(mem, "<script>\"") == NULL && strstr(mem, "&lt;script&gt;") != NULL);
    CHECK(strstr(mem, "data-s=\"Open &quot;Cafe&quot;\"") != NULL);
    CHECK(strstr(mem, "value=\"a&amp;b\"") != NULL);
    CHECK(strstr(mem, "Falsches Passwort?") != NULL);
    CHECK(strstr(mem, "Music-Streamer-AB12") != NULL);
    CHECK(strstr(mem, "http://") == strstr(mem, "http://192.168.4.1/"));   /* no external resources */
    CHECK(strstr(mem, "#C9A45C") != NULL && strstr(mem, "#F2E8D5") != NULL && strstr(mem, "background:#000") != NULL);
    CHECK(strstr(mem, "&#128274;") != NULL);                                /* lock icon */
    CHECK(strstr(mem, "&lt;script&gt;") < strstr(mem, "Open &quot;Cafe"));  /* sorted as given */
    printf("portal form page with 2 networks: %zu bytes\n", b.len);

    pg.kind = NET_PAGE_CONNECTING;
    net_buf_init(&b, mem, sizeof(mem));
    net_page_render(&b, &pg);
    CHECK(!b.overflow && strstr(mem, "http-equiv=refresh") != NULL && strstr(mem, "a&amp;b") != NULL);

    pg.kind = NET_PAGE_CONNECTED;
    pg.ip = "192.168.1.23";
    net_buf_init(&b, mem, sizeof(mem));
    net_page_render(&b, &pg);
    CHECK(!b.overflow && strstr(mem, "192.168.1.23") != NULL && strstr(mem, "Wohnzimmer") != NULL);
    CHECK(strstr(mem, "startet neu") == NULL);
    pg.restarting = true;
    net_buf_init(&b, mem, sizeof(mem));
    net_page_render(&b, &pg);
    CHECK(!b.overflow && strstr(mem, "Verbunden!") != NULL && strstr(mem, "startet neu") != NULL);

    /* small buffer: cut, terminated, flagged */
    static char small[100];
    net_buf_init(&b, small, sizeof(small));
    net_page_render(&b, &pg);
    CHECK(b.overflow && b.len == sizeof(small) - 1 && strlen(small) == b.len);

    net_buf_init(&b, small, sizeof(small));
    net_buf_printf(&b, "%s-%d", "x", 42);
    CHECK(strcmp(small, "x-42") == 0 && !b.overflow);
}

/* NET_PAGES_OUT=<dir>: write the portal pages as HTML files for a look in a browser. */
static void dump_pages(const char *dir)
{
    static char mem[12288];
    net_ap_info_t aps[] = {{"FRITZ!Box 7590 XY", -48, 6, true}, {"Vodafone-ABCD", -61, 11, true},
                           {"Telekom_FON", -70, 1, false}, {"Nachbar 5G", -83, 36, true}};
    const struct {
        const char *file;
        net_page_t pg;
    } pages[] = {
        {"portal_form.html", {.kind = NET_PAGE_FORM, .aps = aps, .n_aps = 4}},
        {"portal_scanning.html", {.kind = NET_PAGE_FORM, .scanning = true}},
        {"portal_error.html", {.kind = NET_PAGE_FORM, .aps = aps, .n_aps = 4, .ssid = "FRITZ!Box 7590 XY",
                               .error = "Falsches Passwort? Bitte noch einmal eingeben (Gro\xc3\x9f-/Kleinschreibung "
                                        "beachten)."}},
        {"portal_connecting.html", {.kind = NET_PAGE_CONNECTING, .ssid = "FRITZ!Box 7590 XY"}},
        {"portal_connected.html", {.kind = NET_PAGE_CONNECTED, .ssid = "FRITZ!Box 7590 XY", .ip = "192.168.178.57",
                                   .restarting = true}},
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        net_page_t pg = pages[i].pg;
        pg.ap_ssid = "Music-Streamer";
        pg.device = "Music Streamer";
        net_buf_t b;
        net_buf_init(&b, mem, sizeof(mem));
        net_page_render(&b, &pg);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, pages[i].file);
        FILE *f = fopen(path, "w");
        if (f) {
            fwrite(mem, 1, b.len, f);
            fclose(f);
            printf("wrote %s (%zu bytes)\n", path, b.len);
        }
    }
}

int main(void)
{
    test_semver();
    test_decide();
    test_hex_url();
    test_text();
    test_qr();
    test_scan();
    test_dns();
#ifndef NET_TEST_NO_CJSON
    test_manifest();
#else
    printf("note: manifest tests skipped (IDF_PATH not set, cJSON not found)\n");
#endif
    test_pages();
    if (getenv("NET_PAGES_OUT")) {
        dump_pages(getenv("NET_PAGES_OUT"));
    }
    if (s_fail) {
        printf("FAIL: %d of %d checks failed\n", s_fail, s_checks);
        return 1;
    }
    printf("PASS: %d checks\n", s_checks);
    return 0;
}
