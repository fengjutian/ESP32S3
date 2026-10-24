/*
 * QMI8658 六轴 IMU（只用到加速度计）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** @brief 初始化 IMU；找不到芯片返回 ESP_ERR_NOT_FOUND。 */
esp_err_t imu_init(void);

/** @brief 芯片是否可用。 */
bool imu_present(void);

/**
 * @brief 读取加速度（单位 g）。
 */
esp_err_t imu_read_accel(float *ax, float *ay, float *az);
