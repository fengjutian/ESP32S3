/*
 * WiFi 配网：NVS 存取账号 + SoftAP 网页表单。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** @brief 从 NVS 读取已保存的账号；两者都为空时返回 false。 */
bool wifi_prov_get_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/** @brief 保存账号到 NVS。 */
bool wifi_prov_save_credentials(const char *ssid, const char *pass);

/** @brief 清除已保存的账号（下次开机会重新配网）。 */
void wifi_prov_clear_credentials(void);

/**
 * @brief 后台启动 SoftAP + 网页配网（不阻塞调用者）。
 *
 * 保存成功后设备会自动重启；超时则自动关闭热点。
 *
 * @param timeout_ms 热点持续时长
 * @return true 已启动
 */
bool wifi_prov_start(int timeout_ms);

/** @brief 配网热点是否正在运行。 */
bool wifi_prov_active(void);
