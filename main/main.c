/*
 * 应用入口：初始化各外设，启动表盘与后台联网任务。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "board_config.h"
#include "board_i2c.h"
#include "bq27220.h"
#include "display.h"
#include "imu.h"
#include "pedometer.h"
#include "rtc.h"
#include "touch.h"
#include "watch.h"
#include "weather.h"
#include "wifi_time.h"

static const char *TAG = "app";

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("Hello world!\n");
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    printf("silicon revision v%u.%u, ", chip_info.revision / 100, chip_info.revision % 100);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
               (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    } else {
        printf("flash size unknown\n");
    }
}

void app_main(void)
{
    log_chip_info();

    /* NVS 是 WiFi 的前提 */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(nvs));
    }

    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed - check main/board_config.h");
        return;
    }

    if (board_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
    }

    if (pcf85063_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC unavailable");
    }

    if (bq27220_init() != ESP_OK) {
        ESP_LOGW(TAG, "fuel gauge unavailable");
    }

    if (imu_init() != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable (pedometer disabled)");
    }
    pedometer_init();

    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable");
    }

    weather_init();
    watch_app_start();
    time_sync_start();
}
