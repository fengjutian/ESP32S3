/*
 * 板载 I2C 总线（触摸、RTC、IMU、音频、电量计共用）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * @brief 初始化板载 I2C 总线（重复调用安全）。
 */
esp_err_t board_i2c_init(void);

/**
 * @brief 在总线上挂一个 7 位地址的设备。
 */
esp_err_t board_i2c_add_device(uint8_t addr, uint32_t scl_hz, i2c_master_dev_handle_t *out);
