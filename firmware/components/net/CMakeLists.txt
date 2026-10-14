# net: WiFi station, setup portal (hotspot + captive DNS + web page), OTA and
# SNTP. The pure helpers in src/pure have no ESP-IDF dependency besides cJSON
# and are unit-tested on the host: firmware/tools/host_test_net.sh
set(embed)
if(CONFIG_MS_OTA_TEST_CA)
    list(APPEND embed "test/ota_test_ca.pem")    # QEMU test build only
endif()

idf_component_register(
    SRCS
        "src/net_wifi.c"
        "src/net_portal.c"
        "src/net_ota.c"
        "src/net_time.c"
        "src/pure/net_dns.c"
        "src/pure/net_manifest.c"
        "src/pure/net_pages.c"
        "src/pure/net_scan.c"
        "src/pure/net_semver.c"
        "src/pure/net_text.c"
        "src/pure/net_wifiqr.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/pure"
    EMBED_TXTFILES ${embed}
    REQUIRES esp_common freertos settings
    PRIV_REQUIRES
        app_update
        esp_app_format
        esp_event
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_netif
        esp_partition
        esp_system
        esp_timer
        esp_wifi
        heap
        json
        log
        lwip
        mbedtls
)
