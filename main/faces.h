/*
 * 表盘接口。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>

#include "rtc.h"
#include "ui.h"

/** @brief 整块重绘指定表盘。 */
void face_render(face_t face, const rtc_time_t *t, bool full);

/** @brief 时间变化时刷新指定表盘的动态部分。 */
void face_tick(face_t face, const rtc_time_t *t, bool minute_changed, bool day_changed);
