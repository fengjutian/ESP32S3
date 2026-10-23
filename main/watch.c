/*
 * 表盘应用：大字号时间 + 秒进度条 + 中文日期，触摸切换 12/24 小时制。
 *
 * 时间来源：
 *   1. 板载 PCF85063 RTC
 *   2. RTC 不可用 / 掉电过，则用编译时间 + 开机后运行时间做软件时钟，
 *      并在 RTC 可用时把时间写回去
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_log.h"

#include "display.h"
#include "display_config.h"
#include "rtc.h"
#include "touch.h"
#include "watch.h"

static const char *TAG = "watch";

/* 版面 */
#define TIME_Y       64
#define TIME_SCALE   5
#define BAR_Y        168
#define BAR_H        8
#define DATE_Y       200
#define DATE_SCALE   2
#define WEEK_Y       250
#define WEEK_SCALE   2
#define HINT_Y       322

static const char *WEEK_CN[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

/* ---------------- 软件时钟（RTC 不可用时兜底） ---------------- */

static int64_t s_base_seconds;   /* 编译时间对应的 Unix 秒 */
static int64_t s_base_boot_us;

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (*m <= 2));
}

static void clock_from_build_time(void)
{
    char mon[4] = {0};
    int day = 1, year = 2026, hh = 0, mm = 0, ss = 0;
    static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);

    unsigned month = 1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, MONTHS[i], 3) == 0) {
            month = (unsigned)(i + 1);
            break;
        }
    }

    s_base_seconds = days_from_civil(year, month, (unsigned)day) * 86400 + hh * 3600 + mm * 60 + ss;
    s_base_boot_us = esp_timer_get_time();
}

static void clock_from_software(rtc_time_t *t)
{
    const int64_t now = s_base_seconds + (esp_timer_get_time() - s_base_boot_us) / 1000000;

    int64_t days = now / 86400;
    int64_t rem = now % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    int year = 0;
    unsigned month = 1;
    unsigned day = 1;
    civil_from_days(days, &year, &month, &day);

    t->year = year;
    t->month = (int)month;
    t->day = (int)day;
    t->hour = (int)(rem / 3600);
    t->minute = (int)((rem % 3600) / 60);
    t->second = (int)(rem % 60);
    t->weekday = pcf85063_weekday(year, (int)month, (int)day);
}

/* ---------------- 绘制 ---------------- */

static void draw_centered(int y, const char *text, uint8_t scale, uint16_t fg)
{
    display_draw_text_utf8((LCD_H_RES - display_text_width_utf8(text, scale)) / 2,
                           y, text, scale, fg, COLOR_BLACK);
}

static void draw_time(const rtc_time_t *t, bool h24)
{
    char buf[16];
    if (h24) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->hour, t->minute);
    } else {
        int h = t->hour % 12;
        if (h == 0) {
            h = 12;
        }
        snprintf(buf, sizeof(buf), "%2d:%02d", h, t->minute);
    }

    display_fill_rect(0, TIME_Y - 8, LCD_H_RES, TIME_SCALE * 16 + 16, COLOR_BLACK);
    draw_centered(TIME_Y, buf, TIME_SCALE, COLOR_WHITE);

    /* 12 小时制时在时间上方显示 上午/下午 */
    display_fill_rect(0, BAR_Y - 28, LCD_H_RES, 22, COLOR_BLACK);
    if (!h24) {
        draw_centered(BAR_Y - 28, (t->hour < 12) ? "上午" : "下午", 1, COLOR_GRAY);
    }
}

static void draw_date(const rtc_time_t *t)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "%d年%d月%d日", t->year, t->month, t->day);
    display_fill_rect(0, DATE_Y - 4, LCD_H_RES, DATE_SCALE * 16 + 8, COLOR_BLACK);
    draw_centered(DATE_Y, buf, DATE_SCALE, COLOR_CYAN);

    display_fill_rect(0, WEEK_Y - 4, LCD_H_RES, WEEK_SCALE * 16 + 8, COLOR_BLACK);
    draw_centered(WEEK_Y, WEEK_CN[t->weekday % 7], WEEK_SCALE, COLOR_GRAY);
}

static void draw_seconds_bar(int second)
{
    const int w = (LCD_H_RES * second) / 59;

    display_fill_rect(0, BAR_Y, LCD_H_RES, BAR_H, COLOR_GRAY);
    if (w > 0) {
        display_fill_rect(0, BAR_Y, w, BAR_H, COLOR_GREEN);
    }
}

/* ---------------- 任务 ---------------- */

static void watch_task(void *arg)
{
    (void)arg;

    touch_point_t pt;
    bool was_pressed = false;
    bool h24 = true;
    const bool rtc_ok = pcf85063_present();
    rtc_time_t now;
    int last_second = -1;
    int last_minute = -1;
    int last_day = -1;

    clock_from_build_time();

    /* RTC 掉电过的话，把编译时间写进去 */
    if (rtc_ok) {
        rtc_time_t probe;
        if (pcf85063_read(&probe) != ESP_OK) {
            clock_from_software(&now);
            if (pcf85063_write(&now) == ESP_OK) {
                ESP_LOGI(TAG, "RTC was invalid, seeded with build time");
            }
        }
    }

    for (;;) {
        bool got_time = false;
        if (rtc_ok && pcf85063_read(&now) == ESP_OK) {
            got_time = true;
        }
        if (!got_time) {
            clock_from_software(&now);
        }

        if (now.second != last_second) {
            last_second = now.second;
            draw_seconds_bar(now.second);
        }

        if (now.minute != last_minute) {
            last_minute = now.minute;
            draw_time(&now, h24);
        }

        if (now.day != last_day) {
            last_day = now.day;
            draw_date(&now);
            draw_time(&now, h24);
        }

        /* 触摸：按下沿切换 12/24 小时制 */
        const bool pressed = touch_read(&pt);
        if (pressed && !was_pressed) {
            h24 = !h24;
            draw_time(&now, h24);
            ESP_LOGI(TAG, "touch at (%u,%u): %s mode", pt.x, pt.y, h24 ? "24h" : "12h");
        }
        was_pressed = pressed;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void watch_app_start(void)
{
    display_clear(COLOR_BLACK);
    draw_centered(HINT_Y, "轻触切换 12/24 小时", 1, COLOR_GRAY);

    xTaskCreate(watch_task, "watch", 4096, NULL, 5, NULL);
}
