/*
 * 计步器（基于 QMI8658 加速度计）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>

/** @brief 从 NVS 恢复今日步数。 */
void pedometer_init(void);

/** @brief 高频调用（建议 20~50Hz），内部自带限速。 */
void pedometer_update(void);

/** @brief 通知当前日期，用于跨天清零。 */
void pedometer_set_date(int y, int m, int d);

int pedometer_steps(void);
int pedometer_goal(void);
