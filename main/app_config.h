/*
 * 应用级配置（WiFi 账号、NTP 服务器、时区）。
 *
 * 注意：这个文件含密码，已加入 .gitignore，不会被提交。
 *       新克隆项目时请从 app_config.example.h 复制一份。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* WiFi 名称（SSID）—— 华为 TC7102 的 2.4G 网络名 */
#define APP_WIFI_SSID       "502"

/* WiFi 密码 */
#define APP_WIFI_PASSWORD   "15295558830"

/* NTP 服务器；国内可用 ntp.aliyun.com / cn.pool.ntp.org */
#define APP_NTP_SERVER      "ntp.aliyun.com"

/* POSIX 时区字符串：CST-8 = 中国标准时间（UTC+8） */
#define APP_TZ              "CST-8"

/* ---- 天气（Open-Meteo，免费且不需要 API key） ---- */
/* 南京（32.0603N, 118.7969E） */
#define WEATHER_CITY            "南京"
#define WEATHER_LAT             32.0603f
#define WEATHER_LON             118.7969f

/* 天气刷新周期（分钟）。每次刷新会临时开 WiFi，约 6 秒 */
#define WEATHER_REFRESH_MIN     30
