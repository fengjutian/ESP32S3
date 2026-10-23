/*
 * 后台联网校时。
 *
 * 流程：STA 连接 WiFi -> 等 IP -> SNTP 同步 -> 写入 PCF85063 -> 关掉 WiFi。
 * 任何一步失败都不影响表盘（RTC 继续走时）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "app_config.h"
#include "rtc.h"
#include "wifi_time.h"

static const char *TAG = "time_sync";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1

#define WIFI_TIMEOUT_MS     20000
#define SNTP_TIMEOUT_MS     20000

static EventGroupHandle_t s_events;
static int s_retry;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        if (++s_retry <= 3) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", ev->reason, s_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "disconnected (reason %d), giving up", ev->reason);
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

/* 扫描一次，打印目标 SSID 是否在 2.4G 频段可见 */
static void scan_ap(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)APP_WIFI_SSID,
        .show_hidden = false,
    };

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGW(TAG, "SSID \"%s\" NOT found in 2.4GHz scan", APP_WIFI_SSID);
        return;
    }

    wifi_ap_record_t *recs = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!recs) {
        return;
    }
    esp_wifi_scan_get_ap_records(&ap_num, recs);

    for (int i = 0; i < ap_num; i++) {
        ESP_LOGI(TAG, "AP \"%s\"  rssi=%d  ch=%d  auth=%d",
                 (const char *)recs[i].ssid, recs[i].rssi,
                 recs[i].primary, recs[i].authmode);
    }
    free(recs);
}

static void time_sync_task(void *arg)
{
    (void)arg;

    if (strlen(APP_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no WiFi credentials in main/app_config.h, skipping sync");
        vTaskDelete(NULL);
        return;
    }

    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = {0};
    memcpy(wifi_cfg.sta.ssid, APP_WIFI_SSID, strlen(APP_WIFI_SSID));
    memcpy(wifi_cfg.sta.password, APP_WIFI_PASSWORD, strlen(APP_WIFI_PASSWORD));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"...", APP_WIFI_SSID);

    scan_ap();
    esp_wifi_connect();

    const EventBits_t bits = xEventGroupWaitBits(s_events,
                                                 WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi connect failed, keep running on RTC");
        esp_wifi_stop();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WiFi connected, waiting for SNTP...");

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_NTP_SERVER);
    if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK &&
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS)) == ESP_OK) {

        setenv("TZ", APP_TZ, 1);
        tzset();

        const time_t epoch = time(NULL);
        struct tm local;
        localtime_r(&epoch, &local);

        rtc_time_t t = {
            .year = local.tm_year + 1900,
            .month = local.tm_mon + 1,
            .day = local.tm_mday,
            .hour = local.tm_hour,
            .minute = local.tm_min,
            .second = local.tm_sec,
        };

        if (pcf85063_write(&t) == ESP_OK) {
            ESP_LOGI(TAG, "RTC updated: %04d-%02d-%02d %02d:%02d:%02d",
                     t.year, t.month, t.day, t.hour, t.minute, t.second);
        } else {
            ESP_LOGW(TAG, "RTC write failed");
        }
    } else {
        ESP_LOGW(TAG, "SNTP timeout, keep running on RTC");
    }

    esp_netif_sntp_deinit();
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi off, sync task done");

    vTaskDelete(NULL);
}

void time_sync_start(void)
{
    xTaskCreate(time_sync_task, "time_sync", 6144, NULL, 4, NULL);
}
