# ui - LVGL screens of the Music Streamer. Depends only on LVGL (and pthread
# for its mutex); the same sources build in the PC simulator (simulator/).
idf_component_register(
    SRCS "src/ui.c"
         "src/ui_screens.c"
         "assets/ui_icons.c"
         "assets/ui_vinyl.c"
         "assets/ui_font_barlow_14.c"
         "assets/ui_font_barlow_18.c"
         "assets/ui_font_barlow_24.c"
         "assets/ui_font_barlow_32.c"
         "assets/ui_font_barlow_56.c"
         "assets/ui_font_barlow_104.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "assets"
    REQUIRES lvgl
    PRIV_REQUIRES pthread
)
