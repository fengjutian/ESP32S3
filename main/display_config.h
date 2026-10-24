/*
 * 屏幕硬件配置 —— 适配 Waveshare ESP32-S3-Touch-LCD-1.85B
 *   LCD : ST77916, QSPI 接口, 360x360, RGB565
 *
 * 换屏幕时只需要改这个文件。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* =====================================================================
 * 1. 分辨率（ST77916 这块屏是 360x360 方形）
 * ===================================================================== */
#define LCD_H_RES               360
#define LCD_V_RES               360

/* =====================================================================
 * 2. QSPI 接线（1.85B 板子的固定连接，照抄即可）
 *    CS=GPIO21  PCLK=GPIO40  D0=46  D1=45  D2=42  D3=41  RST=3  BL=5
 * ===================================================================== */
#define LCD_PIN_CS              21
#define LCD_PIN_PCLK            40
#define LCD_PIN_D0              46
#define LCD_PIN_D1              45
#define LCD_PIN_D2              42
#define LCD_PIN_D3              41
#define LCD_PIN_RST             3
#define LCD_PIN_BL              5

/* =====================================================================
 * 3. 总线参数（官方 BSP 用 80MHz；花屏就降到 40 或 20）
 * ===================================================================== */
#define LCD_SPI_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ      (80 * 1000 * 1000)

/* =====================================================================
 * 4. 初始化序列版本
 *    这块板有两个屏幕版本，靠读寄存器 0x04 自动判断：
 *      00 7F 7F 7F -> v1
 *      00 02 7F 7F -> v2
 *    0 = 自动（推荐），1 / 2 = 强制指定
 * ===================================================================== */
#define LCD_INIT_VERSION        0

/* =====================================================================
 * 5. 显示效果微调
 * ===================================================================== */
#define LCD_INVERT_COLOR        0   /* 反色就改 1 */
#define LCD_MIRROR_X            0
#define LCD_MIRROR_Y            0
#define LCD_SWAP_XY             0

/* =====================================================================
 * 6. 触摸（CST816S，板载 I2C 总线）
 * ===================================================================== */
#define TOUCH_I2C_PORT          I2C_NUM_0
#define TOUCH_PIN_SCL           10
#define TOUCH_PIN_SDA           11
#define TOUCH_PIN_RST           1
#define TOUCH_PIN_INT           4
#define TOUCH_I2C_ADDR          0x15

/* 板载 I2C 总线速率。总线上挂了触摸/RTC/电量计/IMU/音频共 6 个器件，
 * 400kHz 会出现偶发位错误（年份、小时读错），降到 100kHz 稳定。 */
#define BOARD_I2C_HZ            100000
#define TOUCH_I2C_HZ            BOARD_I2C_HZ

/* 触摸坐标和屏幕方向对不上时改这里 */
#define TOUCH_SWAP_XY           0
#define TOUCH_MIRROR_X          0
#define TOUCH_MIRROR_Y          0

