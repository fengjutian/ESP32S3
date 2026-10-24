/*
 * WiFi 链路管理实现。
 *
 * 状态机很简单：
 *   未连接 --connect()--> 已连接 --release() 且 60s 无引用--> 关闭 WiFi
 *
 * 所有 esp_wifi_start/stop 都在 s_conn_mux 里做，避免联网任务和语音任务
 * 同时开关 WiFi 造成 WIFI_EVENT 错乱。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "net_link.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "wifi_prov.h"

static const char *TAG = "netlink";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1

#define KEEPALIVE_MS        60000    /* 引用归零后再撑 60 秒 */
#define MAX_RETRY           3

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_conn_mux;
static esp_netif_t *s_sta_netif;

static volatile bool s_online;
static volatile bool s_wifi_started;
static volatile bool s_expected_stop;
static volatile int s_retry;
static volatile int s_refs;
static volatile int64_t s_last_release_us;
static bool s_inited;

static char s_ssid[33];
static char s_pass[65];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_online = false;
        if (s_expected_stop) {
            return;   /* 我们自己调用了 esp_wifi_stop() */
        }
        const wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        if (++s_retry <= MAX_RETRY) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", ev->reason, s_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "disconnected (reason %d), giving up", ev->reason);
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_online = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

/* 空闲时关掉 WiFi；每 5 秒检查一次，开销可以忽略 */
static void idle_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (s_refs > 0 || !s_online || !s_wifi_started) {
            continue;
        }
        if (esp_timer_get_time() - s_last_release_us < (int64_t)KEEPALIVE_MS * 1000) {
            continue;
        }

        if (xSemaphoreTake(s_conn_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
            continue;
        }
        if (s_refs == 0 && s_online) {
            ESP_LOGI(TAG, "idle %ds, WiFi off", KEEPALIVE_MS / 1000);
            s_expected_stop = true;
            esp_wifi_stop();
            s_wifi_started = false;
            s_online = false;
        }
        xSemaphoreGive(s_conn_mux);
    }
}

esp_err_t net_link_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    s_conn_mux = xSemaphoreCreateMutex();
    if (!s_events || !s_conn_mux) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &h_ip));

    s_last_release_us = esp_timer_get_time();
    s_inited = true;

    xTaskCreate(idle_task, "net_idle", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "ready (keepalive %d s)", KEEPALIVE_MS / 1000);
    return ESP_OK;
}

bool net_link_has_credentials(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    return wifi_prov_get_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
}

bool net_link_online(void)
{
    return s_online;
}

static void build_sta_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->sta.ssid, s_ssid, strnlen(s_ssid, sizeof(cfg->sta.ssid)));
    memcpy(cfg->sta.password, s_pass, strnlen(s_pass, sizeof(cfg->sta.password)));
    cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
}

bool net_link_connect(uint32_t timeout_ms)
{
    if (!s_inited && net_link_init() != ESP_OK) {
        return false;
    }
    if (s_online) {
        return true;
    }

    if (xSemaphoreTake(s_conn_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return false;
    }

    /* 双检：等锁期间别的任务可能已经连上了 */
    if (s_online) {
        xSemaphoreGive(s_conn_mux);
        return true;
    }

    /* 账号：优先 NVS（配网保存），其次 app_config.h 的兜底 */
    if (!wifi_prov_get_credentials(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass))) {
        if (strlen(APP_WIFI_SSID) == 0) {
            ESP_LOGW(TAG, "no credentials");
            xSemaphoreGive(s_conn_mux);
            return false;
        }
        snprintf(s_ssid, sizeof(s_ssid), "%s", APP_WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", APP_WIFI_PASSWORD);
    }

    wifi_config_t wifi_cfg;
    build_sta_config(&wifi_cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    s_retry = 0;
    s_expected_stop = false;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);

    if (!s_wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "wifi start failed: %s", esp_err_to_name(err));
            xSemaphoreGive(s_conn_mux);
            return false;
        }
        s_wifi_started = true;
    }

    ESP_LOGI(TAG, "connecting to \"%s\"...", s_ssid);
    esp_wifi_connect();

    const EventBits_t bits = xEventGroupWaitBits(s_events,
                                                 WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(s_conn_mux);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "online");
        return true;
    }
    ESP_LOGW(TAG, "connect timeout");
    return false;
}

bool net_link_acquire(uint32_t timeout_ms)
{
    __atomic_add_fetch(&s_refs, 1, __ATOMIC_SEQ_CST);
    if (net_link_connect(timeout_ms)) {
        return true;
    }
    net_link_release();
    return false;
}

void net_link_release(void)
{
    int refs = __atomic_sub_fetch(&s_refs, 1, __ATOMIC_SEQ_CST);
    if (refs < 0) {
        __atomic_store_n(&s_refs, 0, __ATOMIC_SEQ_CST);
    }
    s_last_release_us = esp_timer_get_time();
}
