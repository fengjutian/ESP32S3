idf_component_register(SRCS "hello_world_main.c" "display.c" "touch.c" "rtc.c" "board_i2c.c" "watch.c" "bq27220.c" "wifi_time.c" "weather.c"
                       PRIV_REQUIRES spi_flash esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_timer espressif__esp_lcd_st77916 espressif__esp_lcd_touch espressif__esp_lcd_touch_cst816s esp_wifi esp_netif nvs_flash esp_event lwip esp_http_client esp-tls mbedtls
                       INCLUDE_DIRS "")
