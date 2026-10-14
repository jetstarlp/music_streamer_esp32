idf_component_register(
    SRCS "audio_out.c" "fade.c" "pcm_engine.c" "pcm_ring.c" "tone_gen.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_driver_i2s esp_driver_gpio esp_timer
)
