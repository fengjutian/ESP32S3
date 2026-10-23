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
#include "esp_log.h"

#include "display.h"
#include "display_config.h"
#include "touch.h"

static const char *TAG = "app";

/* 屏幕分区：标题区 / 触摸画布 / 状态行 */
#define CANVAS_Y     110
#define CANVAS_H     120
#define LINE_XY_Y    245
#define LINE_TAPS_Y  275
#define LINE_UP_Y    305

/* 画布上画一个十字准星 */
static void draw_crosshair(uint16_t x, uint16_t y)
{
    display_fill_rect(x - 1, y - 8, 3, 17, COLOR_RED);
    display_fill_rect(x - 8, y - 1, 17, 3, COLOR_RED);
}

static void ui_task(void *arg)
{
    (void)arg;

    touch_point_t pt;
    uint32_t taps = 0;
    uint32_t last_sec = UINT32_MAX;
    char line[48];

    for (;;) {
        if (touch_read(&pt)) {
            taps++;

            /* 清画布后重画准星，避免旧点残留 */
            if (pt.y >= CANVAS_Y && pt.y < CANVAS_Y + CANVAS_H) {
                display_fill_rect(0, CANVAS_Y, LCD_H_RES, CANVAS_H, COLOR_BLACK);
                draw_crosshair(pt.x, pt.y);
            }

            snprintf(line, sizeof(line), "x=%3u  y=%3u", pt.x, pt.y);
            display_fill_rect(0, LINE_XY_Y, LCD_H_RES, 18, COLOR_BLACK);
            display_draw_text(16, LINE_XY_Y, line, 1, COLOR_YELLOW, COLOR_BLACK);

            snprintf(line, sizeof(line), "taps: %lu", (unsigned long)taps);
            display_fill_rect(0, LINE_TAPS_Y, LCD_H_RES, 18, COLOR_BLACK);
            display_draw_text(16, LINE_TAPS_Y, line, 1, COLOR_WHITE, COLOR_BLACK);

            printf("touch #%lu: x=%u y=%u\n", (unsigned long)taps, pt.x, pt.y);
        }

        /* 每秒刷新运行时间 */
        uint32_t sec = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
        if (sec != last_sec) {
            last_sec = sec;
            snprintf(line, sizeof(line), "up %lus", (unsigned long)sec);
            display_fill_rect(0, LINE_UP_Y, LCD_H_RES, 18, COLOR_BLACK);
            display_draw_text(16, LINE_UP_Y, line, 1, COLOR_GREEN, COLOR_BLACK);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

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

    /* ---------- 屏幕 ---------- */
    const bool screen_ok = (display_init() == ESP_OK);
    if (screen_ok) {
        display_clear(COLOR_BLACK);

        const char *title = "Hello World";
        display_draw_text((LCD_H_RES - display_text_width(title, 2)) / 2, 16,
                          title, 2, COLOR_WHITE, COLOR_BLACK);

        const char *sub = "ESP32-S3";
        display_draw_text((LCD_H_RES - display_text_width(sub, 2)) / 2, 60,
                          sub, 2, COLOR_CYAN, COLOR_BLACK);

        const char *hint = "tap the canvas";
        display_draw_text((LCD_H_RES - display_text_width(hint, 1)) / 2, 160,
                          hint, 1, COLOR_GRAY, COLOR_BLACK);
    } else {
        printf("LCD init failed - check main/display_config.h\n");
    }

    /* ---------- 触摸 ---------- */
    const bool touch_ok = (touch_init() == ESP_OK);
    if (!touch_ok) {
        printf("touch init failed - check I2C pins in main/display_config.h\n");
        if (screen_ok) {
            display_draw_text(16, LINE_XY_Y, "touch not found", 1, COLOR_RED, COLOR_BLACK);
        }
    }

    if (screen_ok && touch_ok) {
        xTaskCreate(ui_task, "ui", 4096, NULL, 5, NULL);
    }

    /* 串口心跳，保持不变 */
    uint32_t heartbeat = 0;
    while (1) {
        uint32_t uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        printf("Heartbeat #%" PRIu32 ": uptime %" PRIu32 " ms, free heap %" PRIu32 " bytes\n",
               heartbeat, uptime_ms, esp_get_free_heap_size());
        fflush(stdout);
        heartbeat++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
