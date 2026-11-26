idf_component_register(SRCS "src/usb_fm1.c" "src/midi_stream.c" "src/fm1_session.c" INCLUDE_DIRS "include" PRIV_REQUIRES usb log freertos esp_timer heap editor_proto espressif__m5stack_tab5)
