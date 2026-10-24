/*
 * 表盘主任务：软件时钟、触摸手势、表盘切换、背光与省电。
 *
 * 表盘本身的绘制在 faces.c，共享状态和绘图原语在 ui.c。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_log.h"

#include "bq27220.h"
#include "display.h"
#include "faces.h"
#include "rtc.h"
#include "touch.h"
#include "ui.h"
#include "watch.h"
#include "weather.h"
#include "wifi_time.h"

static const char *TAG = "watch";

#define SWIPE_THRESHOLD   60        /* 横向位移超过这个像素才算滑动 */
#define IDLE_DIM_MS       30000     /* 无操作多久降亮度 */
#define DIM_PERCENT       15

/* ---------------- 软件时钟 ---------------- */

static int64_t s_base_seconds;      /* 当前时间的“本地时间”秒基准 */
static int64_t s_base_boot_us;
static portMUX_TYPE s_time_mux = portMUX_INITIALIZER_UNLOCKED;

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

/* 开机先用编译时间兜底，等 SNTP 校准 */
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

    s_base_seconds = days_from_civil(year, month, (unsigned)day) * 86400
                   + hh * 3600 + mm * 60 + ss;
    s_base_boot_us = esp_timer_get_time();
}

static void clock_from_software(rtc_time_t *t)
{
    portENTER_CRITICAL(&s_time_mux);
    const int64_t base = s_base_seconds;
    const int64_t base_us = s_base_boot_us;
    portEXIT_CRITICAL(&s_time_mux);

    const int64_t now = base + (esp_timer_get_time() - base_us) / 1000000;

    int64_t days = now / 86400;
    int64_t rem = now % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    int year = 0;
    unsigned month = 1, day = 1;
    civil_from_days(days, &year, &month, &day);

    t->year = year;
    t->month = (int)month;
    t->day = (int)day;
    t->hour = (int)(rem / 3600);
    t->minute = (int)((rem % 3600) / 60);
    t->second = (int)(rem % 60);
    t->weekday = pcf85063_weekday(year, (int)month, (int)day);
}

void watch_set_time(time_t epoch)
{
    /* 先按当前时区换算成本地时间，否则会显示成 UTC */
    struct tm local;
    localtime_r(&epoch, &local);

    const int64_t civil = days_from_civil(local.tm_year + 1900,
                                          (unsigned)(local.tm_mon + 1),
                                          (unsigned)local.tm_mday) * 86400
                        + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;

    portENTER_CRITICAL(&s_time_mux);
    s_base_seconds = civil;
    s_base_boot_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_time_mux);

    ESP_LOGI(TAG, "clock set to %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

/* ---------------- 手势 ---------------- */

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_RIGHT,
    GESTURE_LONG_PRESS,
} gesture_t;

typedef struct {
    bool pressed;
    uint16_t start_x;
    int max_dx;
    int64_t press_start_us;
} gesture_state_t;

static gesture_t gesture_feed(gesture_state_t *st, bool pressed_now, uint16_t x)
{
    gesture_t result = GESTURE_NONE;

    if (pressed_now) {
        if (!st->pressed) {
            st->start_x = x;
            st->max_dx = 0;
            st->press_start_us = esp_timer_get_time();
        }
        const int dx = (int)x - (int)st->start_x;
        if (abs(dx) > abs(st->max_dx)) {
            st->max_dx = dx;
        }
        st->pressed = true;
        return GESTURE_NONE;
    }

    if (!st->pressed) {
        return GESTURE_NONE;
    }

    st->pressed = false;
    const int64_t held_ms = (esp_timer_get_time() - st->press_start_us) / 1000;

    if (held_ms > 1000) {
        result = GESTURE_LONG_PRESS;
    } else if (st->max_dx <= -SWIPE_THRESHOLD) {
        result = GESTURE_SWIPE_LEFT;
    } else if (st->max_dx >= SWIPE_THRESHOLD) {
        result = GESTURE_SWIPE_RIGHT;
    } else {
        result = GESTURE_TAP;
    }
    st->max_dx = 0;
    return result;
}

/* ---------------- 主任务 ---------------- */

static void watch_task(void *arg)
{
    (void)arg;

    gesture_state_t touch = {0};
    int64_t last_input_us = esp_timer_get_time();
    bool dimmed = false;
    uint32_t last_weather_seq = 0;

    rtc_time_t now;
    int last_second = -1;
    int last_minute = -1;
    int last_day = -1;
    int64_t last_batt_us = 0;

    clock_from_build_time();

    if (bq27220_present()) {
        bq27220_read(&ui_soc, &ui_mv);
    }

    for (;;) {
        clock_from_software(&now);

        /* 电量每 5 秒刷新 */
        const int64_t now_us = esp_timer_get_time();
        if (bq27220_present() && (now_us - last_batt_us > 5000000)) {
            last_batt_us = now_us;
            bq27220_read(&ui_soc, &ui_mv);
            if (ui_face == FACE_CLASSIC) {
                ui_draw_battery_line(310);
            } else if (ui_face == FACE_MINIMAL) {
                ui_draw_battery_line(300);
            }
        }

        const bool sec_changed = (now.second != last_second);
        const bool min_changed = (now.minute != last_minute);
        const bool day_changed = (now.day != last_day);

        if (sec_changed || min_changed || day_changed) {
            face_tick(ui_face, &now, min_changed, day_changed);
            last_second = now.second;
            last_minute = now.minute;
            last_day = now.day;
        }

        if (min_changed) {
            ESP_LOGI(TAG, "now %04d-%02d-%02d %02d:%02d:%02d",
                     now.year, now.month, now.day, now.hour, now.minute, now.second);
        }

        /* 天气数据更新后立刻重画天气盘 */
        const uint32_t wseq = weather_seq();
        if (ui_face == FACE_WEATHER && wseq != last_weather_seq) {
            last_weather_seq = wseq;
            face_render(FACE_WEATHER, &now, true);
        }

        /* 触摸手势 */
        touch_point_t pt;
        const bool pressed_now = touch_read(&pt);
        switch (gesture_feed(&touch, pressed_now, pt.x)) {
        case GESTURE_LONG_PRESS:
            if (ui_face == FACE_WEATHER) {
                net_request_refresh();
                ESP_LOGI(TAG, "weather refresh requested");
            } else {
                ui_h24 = !ui_h24;
                ESP_LOGI(TAG, "long press -> %s", ui_h24 ? "24h" : "12h");
                face_render(ui_face, &now, true);
            }
            break;

        case GESTURE_SWIPE_RIGHT:
            ui_face = (face_t)((ui_face + FACE_COUNT - 1) % FACE_COUNT);
            ESP_LOGI(TAG, "swipe right -> %s", ui_face_name[ui_face]);
            face_render(ui_face, &now, true);
            break;

        case GESTURE_TAP:
        case GESTURE_SWIPE_LEFT:
            ui_face = (face_t)((ui_face + 1) % FACE_COUNT);
            ESP_LOGI(TAG, "%s -> %s",
                     (ui_face == FACE_ANALOG) ? "tap" : "next", ui_face_name[ui_face]);
            face_render(ui_face, &now, true);
            break;

        default:
            break;
        }

        /* 30 秒无操作 → 降亮度 */
        if (pressed_now) {
            last_input_us = esp_timer_get_time();
            if (dimmed) {
                dimmed = false;
                display_set_backlight(100);
                ESP_LOGI(TAG, "backlight: on");
            }
        } else if (!dimmed && (esp_timer_get_time() - last_input_us > IDLE_DIM_MS * 1000LL)) {
            dimmed = true;
            display_set_backlight(DIM_PERCENT);
            ESP_LOGI(TAG, "backlight: dimmed");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void watch_app_start(void)
{
    display_clear(COLOR_BLACK);
    ui_text_center(160, "启动中...", 2, COLOR_GRAY);

    xTaskCreate(watch_task, "watch", 6144, NULL, 5, NULL);
}
