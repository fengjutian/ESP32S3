/*
 * 表盘应用：大字号时间 + 秒进度条 + 中文日期，触摸切换 12/24 小时制。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/**
 * @brief 启动表盘任务（需要先 display_init()、touch_init()）。
 */
void watch_app_start(void);
