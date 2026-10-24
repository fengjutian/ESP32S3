/*
 * 应用级配置（NTP、时区、天气）�? *
 * WiFi 账号不再写死在这里：首次开机会进入配网模式（热�?ESP-Watch），
 * 手机网页填好后存�?NVS，之后自动使用�? *
 * 如果需要临时固定一个账号（比如调试），填下面两个宏即可�? *   APP_WIFI_SSID / APP_WIFI_PASSWORD
 * 它们只是 NVS 为空时的兜底，优先级低于配网保存的账号�? *
 * 注意：如果填了密码，请确保本文件�?.gitignore 里�? *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* WiFi 兜底账号（留�?= 完全依赖配网�?*/
#define APP_WIFI_SSID       ""
#define APP_WIFI_PASSWORD   ""

/* NTP 服务器；国内可用 ntp.aliyun.com / cn.pool.ntp.org */
#define APP_NTP_SERVER      "ntp.aliyun.com"

/* POSIX 时区字符串：CST-8 = 中国标准时间（UTC+8�?*/
#define APP_TZ              "CST-8"

/* ---- 天气（Open-Meteo，免费且不需�?API key�?---- */
/* 南京�?2.0603N, 118.7969E�?*/
#define WEATHER_CITY            "南京"
#define WEATHER_LAT             32.0603f
#define WEATHER_LON             118.7969f

/* 天气刷新周期（分钟）。每次刷新会临时开 WiFi，约 6 �?*/
#define WEATHER_REFRESH_MIN     30

/* 配网开关：
 *   0 = 正常（NVS 有账号就用，没有才开配网热点�? *   1 = 强制配网（忽�?NVS 和上面的兜底账号，开机直接开热点�? * 测试配网时临时改�?1，测完改�?0�? */
#define APP_FORCE_PROVISION     0

/* =====================================================================
 * AI 语音问答（MiniMax�? *   key 在这里填，本文件已在 .gitignore 里，不会进仓库�? *   其余可调参数�?main/ai_config.h�? * ===================================================================== */
#define MINIMAX_API_KEY         "sk-cp-xdDKO0Cc13edT2b_rUjBql7Vi1o7GSW47Xp40KpugWHxZE7hR1wUfBFsZewB__CYfB0VDJVILx8Mr0Ii4VDXUAwNr-3c_bMdzDAn-zWs-kVhMrDqUxh1vWo"
#define MINIMAX_BASE_URL        "https://api.minimax.cn"
#define MINIMAX_TTS_VOICE       "Chinese (Mandarin)_Lyrical_Voice"

/* 开机自检：验证完已关闭（需要时改成 1，串口会�?selftest OK / FAILED�?*/
#define AI_SELFTEST_ON_BOOT     1
