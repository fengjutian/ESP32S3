/*
 * 板载 I2C 总线初始化。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "board_i2c.h"

#include "esp_check.h"
#include "esp_log.h"

#include "display_config.h"

static const char *TAG = "i2c";

static i2c_master_bus_handle_t s_bus;

esp_err_t board_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_PIN_SDA,
        .scl_io_num = TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "i2c bus init failed");

    ESP_LOGI(TAG, "bus ready (SCL=%d SDA=%d)", TOUCH_PIN_SCL, TOUCH_PIN_SDA);
    return ESP_OK;
}

esp_err_t board_i2c_add_device(uint8_t addr, uint32_t scl_hz, i2c_master_dev_handle_t *out)
{
    ESP_RETURN_ON_FALSE(s_bus, ESP_ERR_INVALID_STATE, TAG, "bus not initialized");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_hz,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, out);
}

i2c_master_bus_handle_t board_i2c_handle(void)
{
    return s_bus;
}
