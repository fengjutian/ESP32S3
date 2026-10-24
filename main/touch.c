/*
 * CST816S 电容触摸驱动（基于乐鑫官方 esp_lcd_touch + esp_lcd_touch_cst816s 组件）。
 *
 * 之前手写寄存器解析会读到 f=5 / y=2560 之类的垃圾数据，并因读取偶发失败
 * 产生大量虚假的按下/松开；改用官方组件后协议由组件维护。
 * 另外这里加了抬手去抖：连续 3 次读不到触点才认为手指抬起。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "touch.h"

#include "esp_check.h"
#include "esp_log.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"

#include "board_i2c.h"
#include "board_config.h"

static const char *TAG = "touch";

#define RELEASE_DEBOUNCE    3   /* 连续几次读不到触点才算抬手 */

static esp_lcd_touch_handle_t s_tp;
static bool s_last_pressed;
static uint8_t s_release_cnt;

esp_err_t touch_init(void)
{
    /* CST816S 空闲会自动休眠，此时 I2C 读会 NAK。驱动会打 ERROR 日志刷屏，
     * 这里把它的内部 tag 静音（功能不受影响，触点到来时仍能正常读到）。 */
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("CST816S", ESP_LOG_NONE);

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c bus init failed");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_i2c_handle(), &io_cfg, &tp_io),
                        TAG, "new panel io failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_PIN_RST,
        .int_gpio_num = TOUCH_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = TOUCH_SWAP_XY,
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp),
                        TAG, "cst816s init failed");

    ESP_LOGI(TAG, "CST816S ready (esp_lcd_touch driver)");
    return ESP_OK;
}

bool touch_read(touch_point_t *pt)
{
    if (!s_tp || !pt) {
        return false;
    }

    uint16_t x = 0, y = 0;
    uint8_t count = 0;
    bool touched = false;

    if (esp_lcd_touch_read_data(s_tp) == ESP_OK) {
        touched = esp_lcd_touch_get_coordinates(s_tp, &x, &y, NULL, &count, 1);
    }

    if (touched && count > 0) {
        s_release_cnt = 0;
        s_last_pressed = true;
        pt->x = (x < LCD_H_RES) ? x : (uint16_t)(LCD_H_RES - 1);
        pt->y = (y < LCD_V_RES) ? y : (uint16_t)(LCD_V_RES - 1);
        pt->pressed = true;
        return true;
    }

    /* 抬手去抖：偶发读失败不当作抬手 */
    if (s_last_pressed) {
        if (++s_release_cnt < RELEASE_DEBOUNCE) {
            pt->pressed = true;
            return true;
        }
        s_last_pressed = false;
    }

    pt->pressed = false;
    return false;
}
