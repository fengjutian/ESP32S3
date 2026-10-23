/*
 * CST816S 电容触摸驱动（最小实现，轮询方式读取坐标）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} touch_point_t;

/**
 * @brief 初始化 I2C 总线并复位 CST816S。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示总线上没有应答。
 */
esp_err_t touch_init(void);

/**
 * @brief 读取一次触摸状态。
 *
 * @param[out] pt 坐标与按下状态
 * @return true  本次检测到按下（可用于触发一次点击动作）
 * @return false 没有触摸或读取失败
 */
bool touch_read(touch_point_t *pt);
