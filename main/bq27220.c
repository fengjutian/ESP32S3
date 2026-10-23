/*
 * BQ27220 电量计驱动（只读标准命令，不需要 unseal）。
 *
 * 命令字（2 字节，低字节先发）：
 *   0x08 Voltage()         mV
 *   0x2C StateOfCharge()   %
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "bq27220.h"

#include "esp_check.h"
#include "esp_log.h"

#include "board_i2c.h"

static const char *TAG = "bq27220";

#define BQ27220_ADDR        0x55
#define CMD_VOLTAGE         0x08
#define CMD_SOC             0x2C

static i2c_master_dev_handle_t s_dev;
static bool s_present;

static esp_err_t read_word(uint16_t cmd, uint16_t *value)
{
    const uint8_t w[2] = {(uint8_t)(cmd & 0xFF), (uint8_t)(cmd >> 8)};
    uint8_t r[2] = {0};

    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, w, sizeof(w), r, sizeof(r), 50),
                        TAG, "read cmd 0x%02X failed", cmd);

    *value = (uint16_t)(r[0] | (r[1] << 8));
    return ESP_OK;
}

esp_err_t bq27220_init(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "board i2c init failed");
    ESP_RETURN_ON_ERROR(board_i2c_add_device(BQ27220_ADDR, 400000, &s_dev),
                        TAG, "add BQ27220 failed");

    uint16_t mv = 0;
    if (read_word(CMD_VOLTAGE, &mv) != ESP_OK) {
        s_present = false;
        ESP_LOGW(TAG, "not responding at 0x%02X", BQ27220_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    s_present = true;
    ESP_LOGI(TAG, "BQ27220 ready at 0x%02X, %u mV", BQ27220_ADDR, (unsigned)mv);
    return ESP_OK;
}

bool bq27220_present(void)
{
    return s_present;
}

esp_err_t bq27220_read(int *soc_percent, int *voltage_mv)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t soc = 0;
    uint16_t mv = 0;

    ESP_RETURN_ON_ERROR(read_word(CMD_SOC, &soc), TAG, "SOC read failed");
    ESP_RETURN_ON_ERROR(read_word(CMD_VOLTAGE, &mv), TAG, "voltage read failed");

    if (soc_percent) {
        *soc_percent = (soc > 100) ? 100 : (int)soc;
    }
    if (voltage_mv) {
        *voltage_mv = (int)mv;
    }
    return ESP_OK;
}
