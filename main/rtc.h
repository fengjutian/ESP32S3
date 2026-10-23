/*
 * PCF85063 实时时钟（I2C 0x51）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int year;    /* 2000 ~ 2099 */
    int month;   /* 1 ~ 12 */
    int day;     /* 1 ~ 31 */
    int hour;    /* 0 ~ 23 */
    int minute;  /* 0 ~ 59 */
    int second;  /* 0 ~ 59 */
    int weekday; /* 0 = 周日 */
} rtc_time_t;

/** @brief 初始化 RTC（内部会确保 I2C 总线已初始化）。 */
esp_err_t pcf85063_init(void);

/** @brief RTC 是否可用。 */
bool pcf85063_present(void);

/**
 * @brief 读取时间。
 * @return ESP_ERR_INVALID_STATE 表示振荡器停过、时间不可信。
 */
esp_err_t pcf85063_read(rtc_time_t *t);

/** @brief 写入时间（同时清除停止标志）。 */
esp_err_t pcf85063_write(const rtc_time_t *t);

/** @brief 计算某天的星期（0=周日）。 */
int pcf85063_weekday(int year, int month, int day);
