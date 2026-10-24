/*
 * 后台联网任务：连 WiFi → SNTP 校时 → 拉天气 → 释放（WiFi 保持 60 秒后自动关）。
 *
 * 账号来源优先级：
 *   1. NVS 里保存的（配网写入）
 *   2. app_config.h 里写死的（兼容旧配置）
 *   3. 都没有 → 启动 SoftAP 网页配网，超时则离线运行
 *
 * WiFi 的开关已经抽到 net_link.c：语音问答任务也会 acquire/release 同一条链路。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "app_config.h"
#include "net_link.h"
#include "rtc.h"
#include "watch.h"
#include "weather.h"
#include "wifi_prov.h"
#include "wifi_time.h"

static const char *TAG = "net";

#define WIFI_TIMEOUT_MS     20000
#define SNTP_TIMEOUT_MS     20000
#define PROV_TIMEOUT_MS     180000   /* 配网热点开 3 分钟 */
#define MAX_FAIL_STREAK     3        /* 连续失败几次后重新配网 */

static SemaphoreHandle_t s_refresh_req;

void net_request_refresh(void)
{
    if (s_refresh_req) {
        xSemaphoreGive(s_refresh_req);
    }
}

/* 校时：成功则同时更新软件时钟和 RTC */
static void do_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_NTP_SERVER);
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed");
        return;
    }

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS)) == ESP_OK) {
        setenv("TZ", APP_TZ, 1);
        tzset();

        const time_t epoch = time(NULL);
        struct tm local;
        localtime_r(&epoch, &local);

        watch_set_time(epoch);

        rtc_time_t t = {
            .year = local.tm_year + 1900,
            .month = local.tm_mon + 1,
            .day = local.tm_mday,
            .hour = local.tm_hour,
            .minute = local.tm_min,
            .second = local.tm_sec,
        };
        pcf85063_write(&t);

        ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else {
        ESP_LOGW(TAG, "SNTP timeout");
    }

    esp_netif_sntp_deinit();
}

static void net_task(void *arg)
{
    (void)arg;

    s_refresh_req = xSemaphoreCreateBinary();

    const bool have = !APP_FORCE_PROVISION &&
                      (net_link_has_credentials() || strlen(APP_WIFI_SSID) > 0);
    if (!have) {
        ESP_LOGW(TAG, "no credentials, starting provisioning AP");
        wifi_prov_start(PROV_TIMEOUT_MS);
        vTaskDelay(portMAX_DELAY);   /* 配网成功会重启设备 */
        return;
    }

    int fail_streak = 0;

    for (;;) {
        if (net_link_acquire(WIFI_TIMEOUT_MS)) {
            fail_streak = 0;
            do_sntp();
            weather_fetch();
            net_link_release();
        } else {
            net_link_release();
            ESP_LOGW(TAG, "WiFi connect failed, keep local time");
            if (++fail_streak >= MAX_FAIL_STREAK) {
                ESP_LOGW(TAG, "failed %d times, re-provisioning", fail_streak);
                wifi_prov_start(PROV_TIMEOUT_MS);
                vTaskDelay(portMAX_DELAY);   /* 配网成功会重启设备 */
            }
        }

        ESP_LOGI(TAG, "next refresh in %d min", WEATHER_REFRESH_MIN);

        /* 等下一个周期；期间若有人调用 net_request_refresh() 就立刻重来 */
        if (xSemaphoreTake(s_refresh_req,
                           pdMS_TO_TICKS((uint32_t)WEATHER_REFRESH_MIN * 60u * 1000u)) == pdTRUE) {
            ESP_LOGI(TAG, "manual refresh requested");
        }
    }
}

void time_sync_start(void)
{
    xTaskCreate(net_task, "net", 6144, NULL, 4, NULL);
}
