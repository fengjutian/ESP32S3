/*
 * QMI8658 加速度计驱动。
 *
 * 寄存器（QMI8658A/C 通用）：
 *   0x00 WHO_AM_I  期望 0x05
 *   0x02 CTRL1     0x40 = 地址自增
 *   0x03 CTRL2     0x14 = ±4g, 250Hz
 *   0x08 CTRL7     0x01 = 打开加速度计
 *   0x35 AX_L ... 0x3A AZ_H  小端 int16
 *
 * ±4g 量程下 1g = 8192 LSB。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "imu.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "board_i2c.h"

static const char *TAG = "imu";

#define QMI8658_ADDR_MAIN   0x6B
#define QMI8658_ADDR_ALT    0x6A

#define REG_WHO_AM_I        0x00
#define REG_CTRL1           0x02
#define REG_CTRL2           0x03
#define REG_CTRL7           0x08
#define REG_AX_L            0x35

#define ACCEL_LSB_PER_G     8192.0f

static i2c_master_dev_handle_t s_dev;
static bool s_present;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, 100);
}

static void bus_scan_log(void)
{
    i2c_master_bus_handle_t bus = board_i2c_handle();
    if (!bus) {
        return;
    }

    char line[96] = {0};
    size_t used = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            used += (size_t)snprintf(line + used, sizeof(line) - used, " 0x%02X", addr);
            if (used >= sizeof(line) - 4) {
                break;
            }
        }
    }
    ESP_LOGI(TAG, "I2C devices:%s", used ? line : " (none)");
}

esp_err_t imu_init(void)
{
    board_i2c_init();
    bus_scan_log();

    const uint8_t addrs[2] = {QMI8658_ADDR_MAIN, QMI8658_ADDR_ALT};
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < 2 && !s_present; i++) {
        if (i2c_master_probe(board_i2c_handle(), addrs[i], 20) != ESP_OK) {
            continue;
        }
        if (board_i2c_add_device(addrs[i], BOARD_I2C_HZ, &s_dev) != ESP_OK) {
            continue;
        }

        uint8_t who = 0;
        if (reg_read(REG_WHO_AM_I, &who, 1) == ESP_OK && who == 0x05) {
            s_present = true;
            err = ESP_OK;
            ESP_LOGI(TAG, "QMI8658 found at 0x%02X", addrs[i]);
        } else {
            ESP_LOGW(TAG, "device at 0x%02X has WHO_AM_I=0x%02X (not QMI8658)", addrs[i], who);
        }
    }

    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 地址自增 + 加速度计 ±4g / 250Hz + 使能 */
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL1, 0x40), TAG, "ctrl1 write failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL2, 0x14), TAG, "ctrl2 write failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x01), TAG, "ctrl7 write failed");

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

bool imu_present(void)
{
    return s_present;
}

void imu_dump_registers(void)
{
    if (!s_present) {
        return;
    }

    uint8_t buf[128];
    if (reg_read(0x00, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW(TAG, "register dump failed");
        return;
    }

    for (int row = 0; row < 8; row++) {
        const int base = row * 16;
        char line[96];
        int used = snprintf(line, sizeof(line), "%02X:", base);
        for (int i = 0; i < 16 && used < (int)sizeof(line) - 4; i++) {
            used += snprintf(line + used, sizeof(line) - used, " %02X", buf[base + i]);
        }
        ESP_LOGI(TAG, "%s", line);
    }
}

void imu_log_step_candidates(int soft_steps)
{
    if (!s_present) {
        return;
    }

    uint8_t buf[16];
    if (reg_read(0x58, buf, sizeof(buf)) != ESP_OK) {
        return;
    }

    float ax = 0, ay = 0, az = 0;
    imu_read_accel(&ax, &ay, &az);

    uint8_t raw[6] = {0};
    reg_read(REG_AX_L, raw, sizeof(raw));

    ESP_LOGI(TAG, "accel %+.3f %+.3f %+.3f g | raw %02X%02X %02X%02X %02X%02X | soft=%d",
             (double)ax, (double)ay, (double)az,
             raw[1], raw[0], raw[3], raw[2], raw[5], raw[4], soft_steps);

    ESP_LOGI(TAG, "58:%02X %02X %02X %02X | 60:%02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[8], buf[9], buf[10], buf[11]);
}

esp_err_t imu_read_accel(float *ax, float *ay, float *az)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[6];
    esp_err_t err = reg_read(REG_AX_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    const int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    const int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    const int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    if (ax) *ax = (float)x / ACCEL_LSB_PER_G;
    if (ay) *ay = (float)y / ACCEL_LSB_PER_G;
    if (az) *az = (float)z / ACCEL_LSB_PER_G;
    return ESP_OK;
}
