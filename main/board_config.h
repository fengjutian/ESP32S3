/*
 * 板级硬件配置：屏幕、触摸、I2C 总线。
 *
 * 适配 Waveshare ESP32-S3-Touch-LCD-1.85B
 *   LCD  : ST77916, QSPI, 360x360, RGB565
 *   Touch: CST816S, I2C 0x15
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

/* =====================================================================
 * 1. 屏幕分辨率
 * ===================================================================== */
#define LCD_H_RES               360
#define LCD_V_RES               360

/* =====================================================================
 * 2. 屏幕 QSPI 接线
 *    CS=21  PCLK=40  D0=46  D1=45  D2=42  D3=41  RST=3  BL=5
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
 * 3. 屏幕总线与初始化
 * ===================================================================== */
#define LCD_SPI_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ      (80 * 1000 * 1000)

/* 这块板有两个屏幕版本，靠读寄存器 0x04 自动判断：
 *   00 7F 7F 7F -> v1     00 02 7F 7F -> v2
 * 0 = 自动（推荐），1 / 2 = 强制指定 */
#define LCD_INIT_VERSION        0

/* 显示效果微调 */
#define LCD_INVERT_COLOR        0
#define LCD_MIRROR_X            0
#define LCD_MIRROR_Y            0
#define LCD_SWAP_XY             0

/* =====================================================================
 * 4. 触摸（CST816S，板载 I2C 总线）
 * ===================================================================== */
#define TOUCH_I2C_PORT          I2C_NUM_0
#define TOUCH_PIN_SCL           10
#define TOUCH_PIN_SDA           11
#define TOUCH_PIN_RST           1
#define TOUCH_PIN_INT           4
#define TOUCH_I2C_ADDR          0x15

/* 触摸坐标与屏幕方向不一致时改这里 */
#define TOUCH_SWAP_XY           0
#define TOUCH_MIRROR_X          0
#define TOUCH_MIRROR_Y          0

/* =====================================================================
 * 5. I2C 总线速率
 *    总线上挂触摸/RTC/电量计/IMU/音频共 6 个器件，400kHz 会偶发位错误，
 *    降到 100kHz 稳定。
 * ===================================================================== */
#define BOARD_I2C_HZ            100000

/* =====================================================================
 * 6. 音频（ES8311 喇叭 + ES7210 双麦，共用一组 I2S 时钟）
 *    原理图：MCLK=2 BCLK=48 LRCK=38 DIN=39 DOUT=47 PA=9
 * ===================================================================== */
#define AUDIO_I2S_PORT          I2S_NUM_0
#define AUDIO_PIN_MCLK          2
#define AUDIO_PIN_BCLK          48
#define AUDIO_PIN_LRCK          38
#define AUDIO_PIN_DOUT          47     /* ESP -> ES8311，播放 */
#define AUDIO_PIN_DIN           39     /* ES7210 -> ESP，录音 */
#define AUDIO_PIN_PA            9      /* 功放使能，高电平开 */

#define AUDIO_I2C_ADDR_ES8311   0x30
#define AUDIO_I2C_ADDR_ES7210   0x80
