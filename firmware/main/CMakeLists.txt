idf_component_register(
    SRCS "app_main.c" "app_settings.c" "app_ui.c" "app_cover.c" "bench_util.c" "console.c" "net_qemu.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        board
        display
        ui
        esp_http_client
        mbedtls                 # esp_crt_bundle for the cover download
        settings
        audio_out
        spotify_connect
        net
        nfc
        nfc_tag
        console
        esp_app_format
        esp_driver_gpio
        esp_wifi
        esp_eth                 # only used by the QEMU test build (net_qemu.c)
        esp_netif
        esp_event
        nvs_flash
        esp_psram
        esp_timer
        heap
)
