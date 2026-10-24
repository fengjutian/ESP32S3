/*
 * UI 共享层：表盘枚举、共享状态、文本与行缓冲绘图原语。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"
#include "display.h"
#include "rtc.h"

/* ---------------- 表盘 ---------------- */

typedef enum {
    FACE_ANALOG = 0,   /* 指针：秒环 + 刻度 */
    FACE_ANALOG_MIN,   /* 简约：只留指针 */
    FACE_CLASSIC,      /* 经典：秒环 + 大数字 */
    FACE_MINIMAL,      /* 极简：超大数字 */
    FACE_INFO,         /* 信息 */
    FACE_WEATHER,      /* 天气 */
    FACE_SNAKE,        /* 贪吃蛇 */
    FACE_COUNT
} face_t;

extern const char *const ui_face_name[FACE_COUNT];
extern const char *const ui_week_cn[7];

/* ---------------- 共享状态 ---------------- */

extern face_t ui_face;   /* 当前表盘 */
extern bool ui_h24;      /* true = 24 小时制 */
extern int ui_soc;       /* 电量 %，-1 表示未知 */
extern int ui_mv;        /* 电池电压 mV，-1 表示未知 */

/* ---------------- 表盘几何 ---------------- */

#define UI_RING_CX      180
#define UI_RING_CY      180
#define UI_RING_R       172
#define UI_RING_W       6
#define UI_COLOR_TRACK  0x0000   /* 秒环轨迹色（黑 = 不显示） */

/* ---------------- 文本 ---------------- */

/** @brief 在指定 y 居中绘制一行 UTF-8 文本。 */
void ui_text_center(int y, const char *text, uint8_t scale, uint16_t fg);

/** @brief 格式化为 "HH:MM"（12 小时制时无前导零）。 */
void ui_format_time(const rtc_time_t *t, char *buf, size_t len, bool h24);

/** @brief 绘制 "电池 NN%" 行（自带背景擦除）。 */
void ui_draw_battery_line(int y);

/* ---------------- 行缓冲矢量绘制 ----------------
 * 坐标为屏幕绝对坐标，rx0 是当前行缓冲的起始 x。
 */

void ui_px(uint16_t *buf, int w, int y0, int lines, int rx0, int x, int y, uint16_t c);
void ui_disc(uint16_t *buf, int w, int y0, int lines, int rx0,
             int cx, int cy, int r, uint16_t c);
void ui_line(uint16_t *buf, int w, int y0, int lines, int rx0,
             int x1, int y1, int x2, int y2, int thick, uint16_t c);
void ui_polar(int cx, int cy, int deg, int r, int *x, int *y);
