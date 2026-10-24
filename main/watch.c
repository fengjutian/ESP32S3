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
#include "esp_task_wdt.h"

#include "bq27220.h"
#include "display.h"
#include "faces.h"
#include "imu.h"
#include "pedometer.h"
#include "rtc.h"
#include "snake.h"
#include "touch.h"
#include "ui.h"
#include "voice_ai.h"
#include "watch.h"
#include "weather.h"
#include "wifi_time.h"

static const char *TAG = "watch";

#define SWIPE_THRESHOLD   60        /* 位移超过这个像素才算滑动 */
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

void watch_set_time(time_t epoch){
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

/* ---------------- 提示页（配网引导等） ---------------- */

static volatile bool s_msg_mode;
static volatile bool s_force_redraw;

void watch_show_message(const char *title, const char *line1, const char *line2, const char *line3)
{
    s_msg_mode = true;

    display_set_backlight(100);
    display_clear(COLOR_BLACK);

    if (title) {
        ui_text_center(64, title, 2, COLOR_CYAN);
    }
    if (line1) {
        ui_text_center(140, line1, 1, COLOR_WHITE);
    }
    if (line2) {
        ui_text_center(172, line2, 1, COLOR_WHITE);
    }
    if (line3) {
        ui_text_center(204, line3, 1, COLOR_GRAY);
    }
}

void watch_clear_message(void)
{
    s_msg_mode = false;
    s_force_redraw = true;
}

/* ---------------- 手势 ---------------- */

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
    GESTURE_LONG_PRESS,
} gesture_t;

typedef struct {
    bool pressed;
    uint16_t start_x;
    uint16_t start_y;
    int max_dx;
    int max_dy;
    int64_t press_start_us;
} gesture_state_t;

static gesture_t gesture_feed(gesture_state_t *st, bool pressed_now, uint16_t x, uint16_t y)
{
    gesture_t result = GESTURE_NONE;

    if (pressed_now) {
        if (!st->pressed) {
            st->start_x = x;
            st->start_y = y;
            st->max_dx = 0;
            st->max_dy = 0;
            st->press_start_us = esp_timer_get_time();
        }
        const int dx = (int)x - (int)st->start_x;
        const int dy = (int)y - (int)st->start_y;
        if (abs(dx) > abs(st->max_dx)) {
            st->max_dx = dx;
        }
        if (abs(dy) > abs(st->max_dy)) {
            st->max_dy = dy;
        }
        st->pressed = true;
        return GESTURE_NONE;
    }

    if (!st->pressed) {
        return GESTURE_NONE;
    }

    st->pressed = false;
    const int64_t held_ms = (esp_timer_get_time() - st->press_start_us) / 1000;
    const int ax = abs(st->max_dx);
    const int ay = abs(st->max_dy);

    if (held_ms > 1000) {
        result = GESTURE_LONG_PRESS;
    } else if (ax >= ay) {
        if (st->max_dx <= -SWIPE_THRESHOLD) {
            result = GESTURE_SWIPE_LEFT;
        } else if (st->max_dx >= SWIPE_THRESHOLD) {
            result = GESTURE_SWIPE_RIGHT;
        } else {
            result = GESTURE_TAP;
        }
    } else {
        if (st->max_dy <= -SWIPE_THRESHOLD) {
            result = GESTURE_SWIPE_UP;
        } else if (st->max_dy >= SWIPE_THRESHOLD) {
            result = GESTURE_SWIPE_DOWN;
        } else {
            result = GESTURE_TAP;
        }
    }

    st->max_dx = 0;
    st->max_dy = 0;
    return result;
}

/* ---------------- 主任务 ---------------- */

/* AI 表盘：按住说话，松手发送（表盘任务只做手势判定） */
static bool s_ai_pressed;
static int64_t s_ai_press_us;
static face_t s_ai_last_face = FACE_COUNT;

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

    /* 看门狗：主循环只要卡住超过 TWDT 超时（默认 5s）就自动重启，
     * 避免屏幕停在最后一帧、点什么都没反应。 */
    const bool wdt_ok = (esp_task_wdt_add(NULL) == ESP_OK);

    if (bq27220_present()) {
        bq27220_read(&ui_soc, &ui_mv);
    }

    for (;;) {
        if (wdt_ok) {
            esp_task_wdt_reset();
        }

        if (s_msg_mode) {
            /* 提示页不锁界面：碰一下就退回表盘，后台任务继续跑 */
            touch_point_t p;
            if (touch_read(&p)) {
                watch_clear_message();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        clock_from_software(&now);

        /* 计步：喂日期（跨天清零）+ 采样 IMU */
        pedometer_set_date(now.year, now.month, now.day);
        pedometer_update();

        if (s_force_redraw) {
            s_force_redraw = false;
            face_render(ui_face, &now, true);
        }

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

        /* 临时调试：每 3 秒打印 IMU 候选计步寄存器 */
        static int64_t last_probe_us;
        if (now_us - last_probe_us > 3000000) {
            last_probe_us = now_us;
            imu_log_step_candidates(pedometer_steps());
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

        /* 贪吃蛇需要比 1Hz 更快的节拍 */
        if (ui_face == FACE_SNAKE) {
            snake_poll();
        }

        /* 触摸手势；pt 用 static 保存最后一次按下的坐标，
         * 抬手那一拍 touch_read 不写坐标，用未初始化的栈值会误判点按位置。 */
        static touch_point_t pt;
        const bool pressed_now = touch_read(&pt);
        const gesture_t gesture = gesture_feed(&touch, pressed_now, pt.x, pt.y);

        /* 贪吃蛇：方向键转向、棋盘内轻触暂停、滑动或长按切走 */
        if (ui_face == FACE_SNAKE) {
            switch (gesture) {
            case GESTURE_TAP:
                /* 1. 右上角「退出」优先，任何状态都能走 */
                if (snake_exit_hit(pt.x, pt.y)) {
                    ui_face = (face_t)((ui_face + 1) % FACE_COUNT);
                    ESP_LOGI(TAG, "snake: exit button -> %s", ui_face_name[ui_face]);
                    face_render(ui_face, &now, true);
                    break;
                }
                /* 2. 结束态：任意点按都重开（以前只有棋盘中心点得动） */
                if (snake_is_over()) {
                    ESP_LOGI(TAG, "snake: restart");
                    snake_start();
                    break;
                }
                /* 3. 方向键 */
                if (snake_tap_at(pt.x, pt.y)) {
                    break;
                }
                /* 4. 棋盘内轻触 = 暂停/继续 */
                snake_toggle_pause();
                break;

            case GESTURE_SWIPE_LEFT:
            case GESTURE_SWIPE_RIGHT:
            case GESTURE_SWIPE_UP:
            case GESTURE_SWIPE_DOWN:
            case GESTURE_LONG_PRESS:
                ui_face = (face_t)((ui_face + 1) % FACE_COUNT);
                ESP_LOGI(TAG, "leave game -> %s", ui_face_name[ui_face]);
                face_render(ui_face, &now, true);
                break;

            default:
                break;
            }
        } else if (ui_face == FACE_AI) {
            /* 进入 AI 表盘时手指可能还按着，别把它当成一次录音 */
            if (s_ai_last_face != FACE_AI) {
                s_ai_pressed = pressed_now;
            }

            if (pressed_now && !s_ai_pressed) {
                s_ai_pressed = true;
                s_ai_press_us = esp_timer_get_time();
                voice_ai_ptt_press();
            } else if (!pressed_now && s_ai_pressed) {
                s_ai_pressed = false;
                const int64_t held_ms = (esp_timer_get_time() - s_ai_press_us) / 1000;
                /* 按不到 0.35 秒当误触：取消录音，若手里有上一轮回答就重播 */
                voice_ai_ptt_release(held_ms < 350);
            }

            if (gesture == GESTURE_SWIPE_LEFT || gesture == GESTURE_SWIPE_RIGHT) {
                voice_ai_cancel();
                s_ai_pressed = false;
                ui_face = (gesture == GESTURE_SWIPE_LEFT)
                              ? (face_t)((ui_face + 1) % FACE_COUNT)
                              : (face_t)((ui_face + FACE_COUNT - 1) % FACE_COUNT);
                ESP_LOGI(TAG, "leave ai -> %s", ui_face_name[ui_face]);
                face_render(ui_face, &now, true);
            } else {
                voice_ai_render(&now);
            }
        } else {
            switch (gesture) {
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
                         (gesture == GESTURE_TAP) ? "tap" : "swipe left",
                         ui_face_name[ui_face]);
                face_render(ui_face, &now, true);
                break;

            default:
                break;
            }
        }

        s_ai_last_face = ui_face;

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
