/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include "display.h"
#include "display_config.h"

void app_main(void)
{
    printf("Hello world!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    /* 初始化屏幕：失败也不影响串口部分 */
    const bool screen_ok = (display_init() == ESP_OK);
    if (screen_ok) {
        display_clear(COLOR_BLACK);

        const char *title = "Hello World";
        const uint8_t title_scale = 3;
        display_draw_text((LCD_H_RES - display_text_width(title, title_scale)) / 2, 96,
                          title, title_scale, COLOR_WHITE, COLOR_BLACK);

        const char *subtitle = "ESP32-S3";
        const uint8_t subtitle_scale = 2;
        display_draw_text((LCD_H_RES - display_text_width(subtitle, subtitle_scale)) / 2, 168,
                          subtitle, subtitle_scale, COLOR_CYAN, COLOR_BLACK);
    } else {
        printf("LCD init failed - check the pins in main/display_config.h\n");
    }

    /* Keep running instead of restarting: print a heartbeat every 2 seconds. */
    uint32_t heartbeat = 0;
    char line[64];

    while (1) {
        uint32_t uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t free_heap = esp_get_free_heap_size();

        printf("Heartbeat #%" PRIu32 ": uptime %" PRIu32 " ms, free heap %" PRIu32 " bytes\n",
               heartbeat, uptime_ms, free_heap);
        fflush(stdout);

        if (screen_ok) {
            snprintf(line, sizeof(line), "up %" PRIu32 "s   heap %" PRIu32 "K",
                     uptime_ms / 1000, free_heap / 1024);
            display_fill_rect(0, 260, LCD_H_RES, 20, COLOR_BLACK);
            display_draw_text(20, 260, line, 1, COLOR_GREEN, COLOR_BLACK);
        }

        heartbeat++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
