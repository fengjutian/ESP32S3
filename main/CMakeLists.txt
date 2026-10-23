idf_component_register(SRCS "hello_world_main.c" "display.c" "touch.c" "rtc.c" "board_i2c.c" "watch.c"
                       PRIV_REQUIRES spi_flash esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer espressif__esp_lcd_st77916
                       INCLUDE_DIRS "")
