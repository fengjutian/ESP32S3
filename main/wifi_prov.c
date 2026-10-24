/*
 * WiFi 配网实现。
 *
 * 流程：
 *   1. 设备启动热点 "ESP-Watch"（密码 12345678，开放/无网也可访问）
 *   2. 手机连上热点，浏览器打开 http://192.168.4.1
 *   3. 填写家里的 WiFi 名称和密码，点保存
 *   4. 账号写入 NVS，设备重启并连接
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "wifi_prov.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "watch.h"

static const char *TAG = "prov";

#define NVS_NAMESPACE   "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

#define AP_SSID         "ESP-Watch"
#define AP_PASSWORD     "12345678"

static httpd_handle_t s_server;
static SemaphoreHandle_t s_saved;
static volatile bool s_active;
static char s_pending_ssid[33];
static char s_pending_pass[65];

static const char PAGE_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>手表 WiFi 配网</title>"
    "<style>body{font-family:sans-serif;margin:24px;background:#111;color:#eee}"
    "h2{color:#07e0}label{display:block;margin-top:16px;color:#aaa}"
    "input{width:100%;padding:12px;margin-top:6px;font-size:16px;"
    "border:1px solid #444;border-radius:6px;background:#222;color:#eee;box-sizing:border-box}"
    "button{width:100%;padding:14px;margin-top:24px;font-size:18px;color:#003;"
    "background:#07e0;border:0;border-radius:6px}</style></head><body>"
    "<h2>ESP32 手表配网</h2>"
    "<form method=\"post\" action=\"/save\">"
    "<label>WiFi 名称（SSID）</label><input name=\"ssid\" required maxlength=\"32\">"
    "<label>WiFi 密码</label><input name=\"password\" type=\"password\" maxlength=\"64\">"
    "<button type=\"submit\">保存并连接</button>"
    "</form></body></html>";

static const char PAGE_DONE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<style>body{font-family:sans-serif;margin:24px;background:#111;color:#eee}</style>"
    "</head><body><h2>已保存</h2><p>设备正在重启并连接 WiFi，可以关闭本页面。</p>"
    "</body></html>";

/* ---------------- NVS ---------------- */

bool wifi_prov_get_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    char tmp_ssid[33] = {0};
    char tmp_pass[65] = {0};
    size_t len = sizeof(tmp_ssid);
    const esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, tmp_ssid, &len);
    len = sizeof(tmp_pass);
    const esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, tmp_pass, &len);
    nvs_close(h);

    if (e1 != ESP_OK || tmp_ssid[0] == '\0') {
        return false;
    }
    (void)e2;

    snprintf(ssid, ssid_len, "%s", tmp_ssid);
    snprintf(pass, pass_len, "%s", tmp_pass);
    return true;
}

bool wifi_prov_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "credentials saved for \"%s\"", ssid);
    return err == ESP_OK;
}

void wifi_prov_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "credentials cleared");
    }
}

/* ---------------- HTTP ---------------- */

static void url_decode(char *s)
{
    char *dst = s;
    while (*s) {
        if (*s == '+') {
            *dst++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            const char hex[3] = {s[1], s[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *dst++ = *s++;
        }
    }
    *dst = '\0';
}

/* 从 "a=1&b=2" 里取值，找到返回 true */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    const size_t key_len = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            if (n >= out_len) {
                n = out_len - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[256] = {0};

    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            received = httpd_req_recv(req, body, sizeof(body) - 1);
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
            return ESP_FAIL;
        }
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    form_get(body, "password", pass, sizeof(pass));

    if (!wifi_prov_save_credentials(ssid, pass)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", ssid);
    snprintf(s_pending_pass, sizeof(s_pending_pass), "%s", pass);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_DONE, HTTPD_RESP_USE_STRLEN);

    if (s_saved) {
        xSemaphoreGive(s_saved);
    }
    return ESP_OK;
}

static bool start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return false;
    }

    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &save_uri);
    return true;
}

/* ---------------- 配网流程 ---------------- */

bool wifi_prov_active(void)
{
    return s_active;
}

static void prov_task(void *arg)
{
    const int timeout_ms = (int)(intptr_t)arg;

    s_saved = xSemaphoreCreateBinary();
    s_active = true;

    watch_show_message("WiFi 配网", "1. 手机连接热点 ESP-Watch",
                       "密码 12345678", "2. 浏览器打开 192.168.4.1");
    ESP_LOGW(TAG, "starting provisioning AP \"%s\"", AP_SSID);

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_len = strlen(AP_SSID),
        },
    };
    memcpy(ap_cfg.ap.ssid, AP_SSID, strlen(AP_SSID));
    memcpy(ap_cfg.ap.password, AP_PASSWORD, strlen(AP_PASSWORD));

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    if (!start_http_server()) {
        esp_wifi_stop();
        s_active = false;
        watch_clear_message();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AP ready, waiting for credentials...");
    const bool saved = (xSemaphoreTake(s_saved, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    httpd_stop(s_server);
    s_server = NULL;
    esp_wifi_stop();
    s_active = false;

    if (!saved) {
        ESP_LOGW(TAG, "provisioning timeout");
        watch_clear_message();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "got \"%s\", restarting", s_pending_ssid);
    watch_show_message("配网完成", "正在连接 WiFi...", s_pending_ssid, "");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

bool wifi_prov_start(int timeout_ms)
{
    return xTaskCreate(prov_task, "prov", 4096, (void *)(intptr_t)timeout_ms, 4, NULL) == pdPASS;
}
