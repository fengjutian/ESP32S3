/*
 * 表盘主任务接口。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <time.h>

/** @brief 启动表盘任务（需要先 display_init()、touch_init()）。 */
void watch_app_start(void);

/** @brief 用校时结果校准表盘的软件时钟（UTC 时间戳）。 */
void watch_set_time(time_t epoch);

/** @brief 全屏显示一段提示（会暂停表盘刷新），用于配网引导。 */
void watch_show_message(const char *title, const char *line1, const char *line2, const char *line3);

/** @brief 退出提示模式，恢复表盘刷新。 */
void watch_clear_message(void);
