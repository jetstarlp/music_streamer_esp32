# PN532 NFC reader service. pn532_frame.c and t2t.c are plain C without
# ESP-IDF dependencies (host tests: tools/host_test_nfc.sh).
idf_component_register(
    SRCS "nfc.c" "pn532.c" "pn532_frame.c" "t2t.c"
    INCLUDE_DIRS "include"
    REQUIRES nfc_tag
    PRIV_REQUIRES board esp_driver_uart esp_driver_gpio esp_timer heap
)
