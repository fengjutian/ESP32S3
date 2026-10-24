/*
 * 板载 I2C 总线（触摸、RTC、IMU、音频、电量计共用）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#include "board_config.h"   /* 总线引脚与速率（BOARD_I2C_HZ） */

/**
 * @brief 初始化板载 I2C 总线（重复调用安全）。
 */
esp_err_t board_i2c_init(void);

/**
 * @brief 在总线上挂一个 7 位地址的设备。
 */
esp_err_t board_i2c_add_device(uint8_t addr, uint32_t scl_hz, i2c_master_dev_handle_t *out);

/**
 * @brief 取总线句柄（供 esp_lcd_touch 等组件使用）。
 */
i2c_master_bus_handle_t board_i2c_handle(void);
