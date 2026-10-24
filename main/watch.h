/*
 * 表盘应用：大字号时间 + 秒进度条 + 中文日期，触摸切换 12/24 小时制。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <time.h>

/**
 * @brief 启动表盘任务（需要先 display_init()、touch_init()）。
 */
void watch_app_start(void);

/**
 * @brief 用校时结果校准表盘的软件时钟（由 SNTP 任务调用）。
 *
 * @param epoch  Unix 时间戳（已按本地时区换算过）
 */
void watch_set_time(time_t epoch);
