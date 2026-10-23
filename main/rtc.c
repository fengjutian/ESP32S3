/*
 * PCF85063 实时时钟驱动。
 *
 * 寄存器：
 *   0x00 Control_1  0x01 Control_2  0x02 Seconds(bit7=OS)  0x03 Minutes
 *   0x04 Hours(bit6=12/24)  0x05 Days(weekday)  0x06 Date  0x07 Month(bit7=century)  0x08 Years
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "rtc.h"

#include "esp_check.h"
#include "esp_log.h"

#include "board_i2c.h"

static const char *TAG = "pcf85063";

#define PCF85063_ADDR        0x51
#define REG_CONTROL_1        0x00
#define REG_SECONDS          0x02

#define BIT_OS               (1 << 7)   /* seconds 寄存器：振荡器停止 */
#define BIT_STOP             (1 << 5)   /* Control_1：停止计数 */

static i2c_master_dev_handle_t s_dev;
static bool s_present;

static uint8_t bcd_to_dec(uint8_t v)
{
    return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
}

static uint8_t dec_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

int pcf85063_weekday(int year, int month, int day)
{
    /* Sakamoto 算法，返回 0=Sunday */
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year -= 1;
    }
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

esp_err_t pcf85063_init(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "board i2c init failed");
    ESP_RETURN_ON_ERROR(board_i2c_add_device(PCF85063_ADDR, 400000, &s_dev),
                        TAG, "add PCF85063 failed");

    uint8_t reg = REG_CONTROL_1;
    uint8_t val = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 50) != ESP_OK) {
        s_present = false;
        ESP_LOGW(TAG, "not responding at 0x%02X", PCF85063_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    s_present = true;

    if (val & BIT_STOP) {
        val &= (uint8_t)~BIT_STOP;
        uint8_t w[2] = {REG_CONTROL_1, val};
        i2c_master_transmit(s_dev, w, sizeof(w), 100);
        ESP_LOGW(TAG, "cleared STOP bit");
    }

    ESP_LOGI(TAG, "PCF85063 ready at 0x%02X", PCF85063_ADDR);
    return ESP_OK;
}

bool pcf85063_present(void)
{
    return s_present;
}

esp_err_t pcf85063_read(rtc_time_t *t)
{
    if (!s_dev || !t) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = REG_SECONDS;
    uint8_t b[7] = {0};   /* sec, min, hour, weekday, date, month, year */
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), 50),
                        TAG, "read time failed");

    if (b[0] & BIT_OS) {
        ESP_LOGW(TAG, "OS flag set, raw: %02X %02X %02X %02X %02X %02X %02X",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
        return ESP_ERR_INVALID_STATE;   /* 掉过电，时间不可信 */
    }

    t->second = bcd_to_dec(b[0] & 0x7F);
    t->minute = bcd_to_dec(b[1] & 0x7F);
    t->hour = bcd_to_dec(b[2] & 0x3F);
    t->weekday = bcd_to_dec(b[3] & 0x07);
    t->day = bcd_to_dec(b[4] & 0x3F);
    t->month = bcd_to_dec(b[5] & 0x1F);
    t->year = 2000 + bcd_to_dec(b[6]);
    if (b[5] & 0x80) {
        t->year += 100;
    }

    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->minute > 59 || t->second > 59) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t pcf85063_write(const rtc_time_t *t)
{
    if (!s_dev || !t) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t b[8] = {
        REG_SECONDS,
        dec_to_bcd((uint8_t)t->second),
        dec_to_bcd((uint8_t)t->minute),
        dec_to_bcd((uint8_t)t->hour),
        (uint8_t)(pcf85063_weekday(t->year, t->month, t->day) & 0x07),
        dec_to_bcd((uint8_t)t->day),
        (uint8_t)(dec_to_bcd((uint8_t)t->month) | ((t->year >= 2100) ? 0x80 : 0x00)),
        dec_to_bcd((uint8_t)(t->year % 100)),
    };
    return i2c_master_transmit(s_dev, b, sizeof(b), 100);
}
