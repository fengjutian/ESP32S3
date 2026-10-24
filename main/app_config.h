/*
 * 应用级配置（NTP、时区、天气、AI）。
 *
 * WiFi 账号不再写死在这里：首次开机会进入配网模式（热点 ESP-Watch），
 * 手机网页填好后存进 NVS，之后自动使用。
 *
 * 如果需要临时固定一个账号（比如调试），填下面两个宏即可：
 *   APP_WIFI_SSID / APP_WIFI_PASSWORD
 * 它们只是 NVS 为空时的兜底，优先级低于配网保存的账号。
 *
 * 注意：本文件含密钥，已在 .gitignore 里。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* WiFi 兜底账号（留空 = 完全依赖配网） */
#define APP_WIFI_SSID       ""
#define APP_WIFI_PASSWORD   ""

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

/* 配网开关：
 *   0 = 正常（NVS 有账号就用，没有才开配网热点）
 *   1 = 强制配网（忽略 NVS 和上面的兜底账号，开机直接开热点）
 */
#define APP_FORCE_PROVISION     0

/* =====================================================================
 * AI 语音问答（MiniMax）
 *
 *   sk-cp- 开头的订阅 key 属于国内区，网关要用 https://api.minimax.cn；
 *   海外 key 用 https://api.minimax.io。填错会 401 invalid api key。
 *
 *   其余可调参数（模型、音色、录音时长、麦克风增益）见 main/ai_config.h。
 * ===================================================================== */
#define MINIMAX_API_KEY         "sk-cp-xdDKO0Cc13edT2b_rUjBql7Vi1o7GSW47Xp40KpugWHxZE7hR1wUfBFsZewB__CYfB0VDJVILx8Mr0Ii4VDXUAwNr-3c_bMdzDAn-zWs-kVhMrDqUxh1vWo"
#define MINIMAX_BASE_URL        "https://api.minimax.cn"
#define MINIMAX_TTS_VOICE       "Chinese (Mandarin)_Lyrical_Voice"

/* 开机自检：1 = 启动时跑一次对话 + 流式语音播报并打日志，平时保持 0 */
#define AI_SELFTEST_ON_BOOT     0
