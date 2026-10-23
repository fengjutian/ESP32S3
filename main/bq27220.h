/*
 * BQ27220 电量计（I2C 0x55）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief 初始化（内部确保 I2C 总线已就绪）。 */
esp_err_t bq27220_init(void);

/** @brief 芯片是否在总线上应答。 */
bool bq27220_present(void);

/**
 * @brief 读取电量与电压。
 * @param[out] soc_percent 剩余电量百分比（0~100）
 * @param[out] voltage_mv  电池电压（毫伏）
 */
esp_err_t bq27220_read(int *soc_percent, int *voltage_mv);
