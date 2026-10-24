/*
 * BQ27220 电量计驱动。
 *
 * 只依赖电压寄存器（0x08，mV）——它读数可靠。
 * 芯片自带的 StateOfCharge（0x2C）依赖内部电池建模参数和完整充放电学习，
 * 未配置时会乱报（实测 4.1V 报 12%），所以这里改用电压估算。
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

/* 单节锂电放电曲线（空载电压 → 剩余电量） */
static const struct {
    int mv;
    int pct;
} SOC_CURVE[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 70}, {3800, 60},
    {3750, 50},  {3700, 40}, {3650, 30}, {3600, 20}, {3500, 10},
    {3400, 5},   {3300, 0},
};

#define SOC_POINTS (sizeof(SOC_CURVE) / sizeof(SOC_CURVE[0]))

static int soc_from_mv(int mv)
{
    if (mv >= SOC_CURVE[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < SOC_POINTS; i++) {
        if (mv >= SOC_CURVE[i].mv) {
            const int v0 = SOC_CURVE[i - 1].mv, v1 = SOC_CURVE[i].mv;
            const int p0 = SOC_CURVE[i - 1].pct, p1 = SOC_CURVE[i].pct;
            return p1 + (mv - v1) * (p0 - p1) / (v0 - v1);
        }
    }
    return 0;
}

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
    ESP_RETURN_ON_ERROR(board_i2c_add_device(BQ27220_ADDR, BOARD_I2C_HZ, &s_dev),
                        TAG, "add BQ27220 failed");

    uint16_t mv = 0;
    if (read_word(CMD_VOLTAGE, &mv) != ESP_OK) {
        s_present = false;
        ESP_LOGW(TAG, "not responding at 0x%02X", BQ27220_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    s_present = true;

    uint16_t chip_soc = 0;
    read_word(CMD_SOC, &chip_soc);
    ESP_LOGI(TAG, "BQ27220 ready at 0x%02X: %u mV, 估算 %d%% (芯片自带为 %u%%)",
             BQ27220_ADDR, (unsigned)mv, soc_from_mv((int)mv), (unsigned)chip_soc);
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

    uint16_t mv = 0;
    ESP_RETURN_ON_ERROR(read_word(CMD_VOLTAGE, &mv), TAG, "voltage read failed");

    if (voltage_mv) {
        *voltage_mv = (int)mv;
    }
    if (soc_percent) {
        *soc_percent = soc_from_mv((int)mv);
    }
    return ESP_OK;
}
