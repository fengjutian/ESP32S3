/*
 * CST816S 电容触摸驱动（最小实现）。
 *
 * 寄存器（从 0x01 起连续读 6 字节）：
 *   [0] 0x01 FingerNum   [1] 0x02 XposH   [2] 0x03 XposL
 *   [3] 0x04 YposH       [4] 0x05 YposL   [5] 0x06 Event
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_check.h"
#include "esp_log.h"

#include "board_i2c.h"
#include "display_config.h"

static const char *TAG = "touch";

#define REG_FINGER_NUM      0x01
#define REG_EVENT           0x06

#define EVENT_DOWN          0x00
#define EVENT_UP            0x01
#define EVENT_CONTACT       0x02

static i2c_master_dev_handle_t s_dev;

esp_err_t touch_init(void)
{
    /* CST816S 复位：RST 拉低 10ms，放开后等 50ms */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(board_i2c_add_device(TOUCH_I2C_ADDR, TOUCH_I2C_HZ, &s_dev),
                        TAG, "add CST816S device failed");

    uint8_t reg = REG_FINGER_NUM;
    uint8_t dummy = 0;
    esp_err_t probe = i2c_master_transmit_receive(s_dev, &reg, 1, &dummy, 1, 100);
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "CST816S not responding at 0x%02X (%s)", TOUCH_I2C_ADDR, esp_err_to_name(probe));
        return probe;
    }

    ESP_LOGI(TAG, "CST816S ready at 0x%02X", TOUCH_I2C_ADDR);
    return ESP_OK;
}

bool touch_read(touch_point_t *pt)
{
    if (!s_dev || !pt) {
        return false;
    }

    uint8_t reg = REG_FINGER_NUM;
    uint8_t buf[6] = {0};
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 50) != ESP_OK) {
        return false;
    }

    const uint8_t fingers = buf[0] & 0x0F;
    const uint8_t event = buf[5] & 0x03;

    if (fingers == 0 || event == EVENT_UP) {
        pt->pressed = false;
        return false;
    }
    (void)EVENT_DOWN;
    (void)EVENT_CONTACT;

    uint16_t x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    uint16_t y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);

#if TOUCH_SWAP_XY
    uint16_t t = x;
    x = y;
    y = t;
#endif
#if TOUCH_MIRROR_X
    x = (uint16_t)((LCD_H_RES - 1) - x);
#endif
#if TOUCH_MIRROR_Y
    y = (uint16_t)((LCD_V_RES - 1) - y);
#endif

    if (x >= LCD_H_RES) x = LCD_H_RES - 1;
    if (y >= LCD_V_RES) y = LCD_V_RES - 1;

    pt->x = x;
    pt->y = y;
    pt->pressed = true;
    return true;
}
