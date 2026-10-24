/*
 * 极简 ST7789 文本显示接口（不依赖 LVGL，只用 esp_lcd + 内置 8x16 字模）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* RGB565 常用颜色 */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_GRAY    0x8410
#define COLOR_ORANGE  0xFD20

/**
 * @brief 初始化 SPI 总线 + ST7789 面板并点亮背光。
 *
 * @return ESP_OK 成功；其它值表示 SPI/面板初始化失败（此时程序仍可继续跑串口输出）。
 */
esp_err_t display_init(void);

/**
 * @brief 打开/关闭背光（LCD_PIN_BL 为 -1 时该函数无副作用）。
 */
void display_backlight(bool on);

/**
 * @brief 设置背光亮度（0~100，使用 LEDC PWM）。
 */
void display_set_backlight(int percent);

/**
 * @brief 用纯色填充一个矩形（会自动裁剪到屏幕范围内）。
 */
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief 整屏填充。
 */
void display_clear(uint16_t color);

/**
 * @brief 在 (x, y) 处画一行 ASCII 文本。
 *
 * @param scale 放大倍数，1 = 8x16 像素，2 = 16x32 像素
 * @param fg    文字颜色（RGB565）
 * @param bg    背景颜色（RGB565）
 */
void display_draw_text(int x, int y, const char *text, uint8_t scale, uint16_t fg, uint16_t bg);

/**
 * @brief 计算一行文本的像素宽度（用于居中排版）。
 */
int display_text_width(const char *text, uint8_t scale);

/**
 * @brief 绘制 UTF-8 文本，ASCII 用 8x16 字模，中文用 16x16 点阵。
 *
 * 字形宽度：ASCII 8*scale，中文 16*scale（一个汉字）。未知字符画 '?'。
 */
void display_draw_text_utf8(int x, int y, const char *text, uint8_t scale, uint16_t fg, uint16_t bg);

/**
 * @brief 计算 UTF-8 文本的像素宽度。
 */
int display_text_width_utf8(const char *text, uint8_t scale);

/**
 * @brief 画一段圆环（用于秒针进度）。
 *
 * @param cx, cy     圆心
 * @param radius     外半径
 * @param thickness  环宽
 * @param start_deg  起始角度（0 = 12 点方向，顺时针为正）
 * @param end_deg    结束角度
 * @param fg         进度段颜色
 * @param track      未走过部分的颜色
 * @param bg         环外区域颜色
 */
void display_draw_ring(int cx, int cy, int radius, int thickness,
                       int start_deg, int end_deg,
                       uint16_t fg, uint16_t track, uint16_t bg);

/**
 * @brief 只重画圆环的一小段扇形（用于秒针增量刷新，避免整屏重绘）。
 *
 * 只刷新该扇形的包围盒，环外填 bg、环内但不在角度区间填 track。
 */
void display_draw_arc(int cx, int cy, int radius, int thickness,
                      int a0, int a1,
                      uint16_t fg, uint16_t track, uint16_t bg);

/**
 * @brief 取内部行缓冲，用于应用层自行合成画面。
 *
 * 缓冲容量为 LCD_H_RES × LINE_CHUNK 个像素（LINE_CHUNK = 16）。
 * 用法：逐块填充后调用 display_band_push() 刷新到屏幕。
 */
#define DISPLAY_BAND_LINES 16

uint16_t *display_band_buffer(void);

/**
 * @brief 把行缓冲推送到屏幕。
 *
 * @param x0    起始 x
 * @param y     起始 y
 * @param w     宽度（像素，≤ LCD_H_RES）
 * @param lines 行数（≤ 16）
 */
void display_band_push(int x0, int y, int w, int lines);

