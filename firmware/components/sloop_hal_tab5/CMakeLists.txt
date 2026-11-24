idf_component_register(
    SRCS "src/storage_ram.c" "src/audio_stream.c" "src/storage_flash.c" "src/sys_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_codec_dev
    PRIV_REQUIRES heap espressif__m5stack_tab5 driver esp_timer esp_partition nvs_flash tab5_keyboard
)
