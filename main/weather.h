/*
 * 天气数据（Open-Meteo，无需 API key）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool valid;      /* 是否已拿到有效数据 */
    int temp_c;      /* 当前温度（摄氏度，四舍五入） */
    int feels_c;     /* 体感温度 */
    int humidity;    /* 相对湿度 % */
    int wind_kmh;    /* 风速 km/h */
    int tmax_c;      /* 今日最高 */
    int tmin_c;      /* 今日最低 */
    int code;        /* WMO 天气代码 */
    bool is_day;     /* 是否白天 */
} weather_info_t;

/** @brief 初始化（创建互斥锁）。 */
void weather_init(void);

/** @brief 取一份天气快照（线程安全）。 */
void weather_get(weather_info_t *out);

/** @brief 发起一次 HTTP 请求并更新缓存。需要在 WiFi 已连接时调用。 */
esp_err_t weather_fetch(void);

/** @brief WMO 天气代码 → 中文描述。 */
const char *weather_text(int code);

/** @brief 数据版本号，每次拉取成功 +1（用于触发界面重画）。 */
uint32_t weather_seq(void);
