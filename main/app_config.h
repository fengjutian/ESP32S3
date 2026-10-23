/*
 * 应用级配置（WiFi 账号、NTP 服务器、时区）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* 把下面两行填成你的 WiFi；留空则跳过联网校时，表盘继续用 RTC 走时 */
#define APP_WIFI_SSID       ""
#define APP_WIFI_PASSWORD   ""

/* NTP 服务器；国内可用 ntp.aliyun.com / cn.pool.ntp.org */
#define APP_NTP_SERVER      "ntp.aliyun.com"

/* POSIX 时区字符串：CST-8 = 中国标准时间（UTC+8） */
#define APP_TZ              "CST-8"
