/*
 * 天气数据拉取（Open-Meteo）。
 *
 * 请求：
 *   https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..
 *     &current=temperature_2m,relative_humidity_2m,weather_code,is_day
 *
 * 响应里 current 段形如：
 *   "current":{"time":"...","interval":900,"temperature_2m":21.3,
 *              "relative_humidity_2m":68,"weather_code":3,"is_day":0}
 *
 * 只做最小解析（找到 "current":{ 段后按 key 取值），不引入 JSON 库。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_config.h"

static const char *TAG = "weather";

static weather_info_t s_info;
static SemaphoreHandle_t s_mux;
static volatile uint32_t s_seq;

uint32_t weather_seq(void)
{
    return s_seq;
}

void weather_init(void)
{
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutex();
    }
}

void weather_get(weather_info_t *out)
{
    if (!out) {
        return;
    }
    if (s_mux) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
    }
    *out = s_info;
    if (s_mux) {
        xSemaphoreGive(s_mux);
    }
}

const char *weather_text(int code)
{
    switch (code) {
    case 0:  return "晴";
    case 1:  return "少云";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45:
    case 48: return "雾";
    case 51:
    case 53:
    case 55: return "毛毛雨";
    case 56:
    case 57: return "冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66:
    case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80: return "阵雨";
    case 81: return "强阵雨";
    case 82: return "暴雨";
    case 85:
    case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96:
    case 99: return "雷暴冰雹";
    default: return "未知";
    }
}

/* 在某个 JSON 段里取 key 的第一个数值（兼容标量与数组两种写法） */
static bool json_first_number(const char *section, const char *key, double *out)
{
    if (!section) {
        return false;
    }

    char pattern[48];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    p = strstr(section, pattern);
    if (p) {
        p += strlen(pattern);
    } else {
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);
        p = strstr(section, pattern);
        if (!p) {
            return false;
        }
        p += strlen(pattern);
    }

    while (*p == ' ') {
        p++;
    }
    char *end = NULL;
    const double v = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

esp_err_t weather_fetch(void)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,is_day,"
             "apparent_temperature,wind_speed_10m"
             "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
             (double)WEATHER_LAT, (double)WEATHER_LON);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);

    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        const int r = esp_http_client_read(client, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGW(TAG, "empty response");
        return ESP_FAIL;
    }

    const char *cur = strstr(buf, "\"current\":{");
    const char *day_sec = strstr(buf, "\"daily\":{");

    double temp = 0, hum = 0, code = 0, isday = 0;
    double feels = 0, wind = 0, tmax = 0, tmin = 0;

    if (!cur ||
        !json_first_number(cur, "temperature_2m", &temp) ||
        !json_first_number(cur, "weather_code", &code)) {
        ESP_LOGW(TAG, "parse failed, body: %.120s", buf);
        return ESP_FAIL;
    }

    json_first_number(cur, "relative_humidity_2m", &hum);
    json_first_number(cur, "is_day", &isday);
    json_first_number(cur, "apparent_temperature", &feels);
    json_first_number(cur, "wind_speed_10m", &wind);

    if (day_sec) {
        json_first_number(day_sec, "temperature_2m_max", &tmax);
        json_first_number(day_sec, "temperature_2m_min", &tmin);
    }

    if (s_mux) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
    }
    s_info.valid = true;
    s_info.temp_c = (int)(temp >= 0 ? temp + 0.5 : temp - 0.5);
    s_info.feels_c = (int)(feels >= 0 ? feels + 0.5 : feels - 0.5);
    s_info.humidity = (int)(hum + 0.5);
    s_info.wind_kmh = (int)(wind + 0.5);
    s_info.tmax_c = (int)(tmax >= 0 ? tmax + 0.5 : tmax - 0.5);
    s_info.tmin_c = (int)(tmin >= 0 ? tmin + 0.5 : tmin - 0.5);
    s_info.code = (int)code;
    s_info.is_day = (isday > 0.5);
    if (s_mux) {
        xSemaphoreGive(s_mux);
    }

    s_seq++;

    ESP_LOGI(TAG, "%s %d℃ %s, 湿度 %d%%, 风 %dkm/h, 今日 %d~%d℃",
             WEATHER_CITY, s_info.temp_c, weather_text(s_info.code),
             s_info.humidity, s_info.wind_kmh, s_info.tmin_c, s_info.tmax_c);
    return ESP_OK;
}
