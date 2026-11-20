idf_component_register(
    SRCS "src/ui.c" "src/page_play.c" "src/page_tracks.c" "src/page_step.c" "src/page_transport.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__m5stack_tab5 esp_lvgl_port sloop_state sloop_core
)
