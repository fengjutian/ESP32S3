/*
 * CST816S 电容触摸驱动（基于 esp_lcd_touch 组件）。
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
 * @brief 初始化触摸（内部会确保 I2C 总线已就绪）。
 */
esp_err_t touch_init(void);

/**
 * @brief 读取一次触摸状态（含抬手去抖）。
 *
 * @param[out] pt 坐标与按下状态
 * @return true  当前处于按下状态
 * @return false 已抬起或读取失败
 */
bool touch_read(touch_point_t *pt);
