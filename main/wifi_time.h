/*
 * 后台联网校时：连 WiFi -> SNTP 取时 -> 写入 PCF85063 -> 断开 WiFi。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/**
 * @brief 启动后台校时任务（不阻塞调用者）。
 *
 * 未配置 WiFi 账号时直接跳过；连不上或超时也只记录日志，不影响表盘。
 */
void time_sync_start(void);
