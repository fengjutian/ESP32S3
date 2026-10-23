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
#include "nvs_flash.h"

#include "board_i2c.h"
#include "bq27220.h"
#include "display.h"
#include "display_config.h"
#include "rtc.h"
#include "touch.h"
#include "watch.h"
#include "wifi_time.h"

static const char *TAG = "app";

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

    /* NVS 是 WiFi 的前提 */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(nvs));
    }

    /* 屏幕 */
    if (display_init() != ESP_OK) {
        printf("LCD init failed - check main/display_config.h\n");
        return;
    }

    /* I2C 总线：触摸 / RTC / 电量计共用 */
    if (board_i2c_init() != ESP_OK) {
        printf("I2C bus init failed\n");
    }

    if (pcf85063_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC unavailable, using build-time clock");
    }

    if (bq27220_init() != ESP_OK) {
        ESP_LOGW(TAG, "fuel gauge unavailable");
    }

    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable");
    }

    /* 表盘 */
    watch_app_start();

    /* 后台联网校时（没配 WiFi 就自动跳过） */
    time_sync_start();
}
