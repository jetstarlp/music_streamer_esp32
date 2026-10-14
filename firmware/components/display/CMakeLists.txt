# Managed dependencies (esp_lcd_co5300, esp_lvgl_port, lvgl) come from
# idf_component.yml; lvgl is a public dependency there.
idf_component_register(
    SRCS "display.c" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_lcd
    PRIV_REQUIRES board esp_driver_spi esp_timer heap
)

# lv_mem_psram.c is only referenced from LVGL, which links before this
# library: pull the object in explicitly.
if(CONFIG_LV_USE_CUSTOM_MALLOC)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
endif()
