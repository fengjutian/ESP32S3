/*
 * 后台联网任务：连 WiFi -> SNTP 校时 -> 拉天气 -> 断开 WiFi，周期重复。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/**
 * @brief 启动后台联网任务（不阻塞调用者）。
 *
 * 未配置 WiFi 账号时直接跳过；连不上或超时也只记录日志，不影响表盘。
 */
void time_sync_start(void);

/**
 * @brief 请求立即刷新一次（唤醒后台任务，不必等 30 分钟周期）。
 */
void net_request_refresh(void);
