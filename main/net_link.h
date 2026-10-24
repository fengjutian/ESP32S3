/*
 * WiFi 链路管理：全局只初始化一次 esp_wifi，按「引用计数」保持在线。
 *
 * 与旧版 wifi_time.c 的差别：
 *   - 旧版连上 → 干完活 → 立刻关 WiFi；
 *   - 现在最后一次 release() 之后还会保持 60 秒（KEEPALIVE_MS），
 *     这样语音问答的「识别 → 大模型 → 合成」三次请求不必反复重连，
 *     连续追问也是秒回。
 *
 * 典型用法：
 *     if (net_link_acquire(20000)) { ... 发 HTTPS 请求 ... }
 *     net_link_release();
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief 初始化 netif / 事件循环 / esp_wifi。必须在 app_main 里调用一次。 */
esp_err_t net_link_init(void);

/** @brief 连接 WiFi（已连上则立即返回 true）。 */
bool net_link_connect(uint32_t timeout_ms);

/** @brief 引用计数 +1，并确保 WiFi 在线。 */
bool net_link_acquire(uint32_t timeout_ms);

/** @brief 引用计数 -1；归零后延迟关 WiFi（省电）。 */
void net_link_release(void);

/** @brief 当前是否已拿到 IP。 */
bool net_link_online(void);

/** @brief NVS 里是否有配网保存的账号。 */
bool net_link_has_credentials(void);
