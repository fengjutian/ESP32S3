/*
 * 应用级配置模板。
 *
 * 复制成 app_config.h 后按需修改：
 *     copy main\app_config.example.h main\app_config.h
 *
 * WiFi 账号通常不需要填：首次开机会进入网页配网，账号存 NVS。
 * 这里填了只作为「NVS 为空」时的兜底。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* WiFi 兜底账号（留空 = 依赖网页配网） */
#define APP_WIFI_SSID       ""
#define APP_WIFI_PASSWORD   ""

/* NTP 服务器；国内可用 ntp.aliyun.com / cn.pool.ntp.org */
#define APP_NTP_SERVER      "ntp.aliyun.com"

/* POSIX 时区字符串：CST-8 = 中国标准时间（UTC+8） */
#define APP_TZ              "CST-8"

/* 天气：城市名 + 经纬度（Open-Meteo，无需 API key） */
#define WEATHER_CITY            "南京"
#define WEATHER_LAT             32.0603f
#define WEATHER_LON             118.7969f
#define WEATHER_REFRESH_MIN     30

/* 0 = 正常；1 = 强制每次开机都进配网（测试用） */
#define APP_FORCE_PROVISION     0
