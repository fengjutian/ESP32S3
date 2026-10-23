/*
 * 应用级配置模板。
 *
 * 复制成 app_config.h 后填入自己的 WiFi 信息：
 *     copy main\app_config.example.h main\app_config.h
 *
 * app_config.h 已加入 .gitignore，密码不会进仓库。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* WiFi 名称（SSID），必须是 2.4GHz 网络 */
#define APP_WIFI_SSID       ""

/* WiFi 密码 */
#define APP_WIFI_PASSWORD   ""

/* NTP 服务器；国内可用 ntp.aliyun.com / cn.pool.ntp.org */
#define APP_NTP_SERVER      "ntp.aliyun.com"

/* POSIX 时区字符串：CST-8 = 中国标准时间（UTC+8） */
#define APP_TZ              "CST-8"
