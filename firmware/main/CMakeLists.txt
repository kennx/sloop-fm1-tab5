idf_component_register(SRCS "main.c" "display.c" "console.c" INCLUDE_DIRS "." PRIV_REQUIRES espressif__m5stack_tab5 esp_lvgl_port esp_lcd spi_flash console driver esp_driver_usb_serial_jtag)
