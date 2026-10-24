/*
 * 表盘应用：
 *   - 三种表盘，左右滑动切换，轻触切换 12/24 小时制
 *   - 经典表盘：外圈秒针圆环 + 大字号时间 + 中文日期
 *   - 极简表盘：超大时间 + 秒数
 *   - 信息表盘：电量 / 电压 / 时间 / 日期 / 运行时长
 *
 * 时间来源：PCF85063 RTC；掉电过则用编译时间兜底。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_log.h"

#include "bq27220.h"
#include "display.h"
#include "display_config.h"
#include "app_config.h"
#include "rtc.h"
#include "touch.h"
#include "watch.h"
#include "weather.h"
#include "wifi_time.h"

static const char *TAG = "watch";

#define SWIPE_THRESHOLD   60      /* 滑动超过这个像素才算换表盘 */

#define RING_CX           180
#define RING_CY           180
#define RING_R            172
#define RING_W            6

#define COLOR_TRACK       0x0000  /* 轨迹不显示，只留绿色进度弧 */

typedef enum {
    FACE_ANALOG = 0,     /* 指针：秒环 + 刻度 */
    FACE_ANALOG_MIN,     /* 简约：只留指针 */
    FACE_CLASSIC,        /* 经典：秒环 + 大数字 */
    FACE_MINIMAL,        /* 极简：超大数字 */
    FACE_INFO,           /* 信息 */
    FACE_WEATHER,        /* 天气 */
    FACE_COUNT
} face_t;

static const char *WEEK_CN[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
static const char *FACE_NAME[FACE_COUNT] = {"指针", "简约", "经典", "极简", "信息", "天气"};

static face_t s_face = FACE_CLASSIC;
static bool s_h24 = true;
static int s_soc = -1;
static int s_mv = -1;

/* ---------------- 软件时钟兜底 ---------------- */

static int64_t s_base_seconds;   /* 当前时间的 Unix 秒基准 */
static int64_t s_base_boot_us;
static portMUX_TYPE s_time_mux = portMUX_INITIALIZER_UNLOCKED;

static int64_t days_from_civil(int y, unsigned m, unsigned d);

/* 由 SNTP 校时任务调用：把软件时钟对准到指定时刻（Unix 时间戳，UTC） */
void watch_set_time(time_t epoch)
{
    /* 先按当前时区换算成“本地时间”，否则会显示成 UTC */
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

/* ---------------- 绘制工具 ---------------- */

static void text_center(int y, const char *text, uint8_t scale, uint16_t fg)
{
    display_draw_text_utf8((LCD_H_RES - display_text_width_utf8(text, scale)) / 2,
                           y, text, scale, fg, COLOR_BLACK);
}

static void format_time(const rtc_time_t *t, char *buf, size_t len, bool h24)
{
    if (h24) {
        snprintf(buf, len, "%02d:%02d", t->hour, t->minute);
    } else {
        int h = t->hour % 12;
        if (h == 0) {
            h = 12;
        }
        snprintf(buf, len, "%2d:%02d", h, t->minute);
    }
}

static void format_battery(char *buf, size_t len)
{
    if (s_soc < 0) {
        snprintf(buf, len, "电池 --");
    } else {
        snprintf(buf, len, "电池 %d%%", s_soc);
    }
}

/* 电池行统一从这里画，避免不同调用点用不同 y 导致重影 */
static void draw_battery_line(int y)
{
    char buf[32];

    format_battery(buf, sizeof(buf));
    display_fill_rect(0, y - 6, LCD_H_RES, 22, COLOR_BLACK);
    text_center(y, buf, 1, COLOR_GRAY);
}

/* ---------------- 各表盘 ---------------- */

/* ---- 指针表盘：全部在一个行缓冲里合成，避免指针互相擦除 ---- */

static inline void buf_px(uint16_t *buf, int w, int y0, int lines, int rx0, int x, int y, uint16_t c)
{
    const int row = y - y0;
    const int col = x - rx0;
    if (col < 0 || col >= w || row < 0 || row >= lines) {
        return;
    }
    buf[row * w + col] = c;
}

static void buf_disc(uint16_t *buf, int w, int y0, int lines, int rx0,
                     int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                buf_px(buf, w, y0, lines, rx0, cx + dx, cy + dy, c);
            }
        }
    }
}

static void buf_line(uint16_t *buf, int w, int y0, int lines, int rx0,
                     int x1, int y1, int x2, int y2, int thick, uint16_t c)
{
    const int dx = x2 - x1;
    const int dy = y2 - y1;
    int steps = (dx > 0) ? dx : -dx;
    const int ady = (dy > 0) ? dy : -dy;
    if (ady > steps) {
        steps = ady;
    }
    if (steps == 0) {
        buf_disc(buf, w, y0, lines, rx0, x1, y1, thick / 2, c);
        return;
    }

    const int r = thick / 2;
    for (int i = 0; i <= steps; i++) {
        const int x = x1 + dx * i / steps;
        const int y = y1 + dy * i / steps;
        if (r <= 0) {
            buf_px(buf, w, y0, lines, rx0, x, y, c);
        } else {
            buf_disc(buf, w, y0, lines, rx0, x, y, r, c);
        }
    }
}

static void polar(int cx, int cy, int deg, int r, int *x, int *y)
{
    const float a = (float)deg * 3.14159265f / 180.0f;
    *x = cx + (int)(sinf(a) * (float)r);
    *y = cy - (int)(cosf(a) * (float)r);
}

#define ANALOG_HOUR_LEN  78
#define ANALOG_MIN_LEN   112
#define ANALOG_SEC_LEN   130
#define ANALOG_HOUR_TH   9
#define ANALOG_MIN_TH    6
#define ANALOG_SEC_TH    3

static bool s_analog_valid;
static rtc_time_t s_analog_prev;

static void grow_hand_bbox(int *bx0, int *by0, int *bx1, int *by1,
                           int cx, int cy, int deg, int len, int thick)
{
    int tx, ty;
    polar(cx, cy, deg, len, &tx, &ty);

    const int x0 = ((cx < tx) ? cx : tx) - thick;
    const int x1 = ((cx > tx) ? cx : tx) + thick;
    const int y0 = ((cy < ty) ? cy : ty) - thick;
    const int y1 = ((cy > ty) ? cy : ty) + thick;

    if (x0 < *bx0) *bx0 = x0;
    if (y0 < *by0) *by0 = y0;
    if (x1 > *bx1) *bx1 = x1;
    if (y1 > *by1) *by1 = y1;
}

static void grow_arc_bbox(int *bx0, int *by0, int *bx1, int *by1,
                          int cx, int cy, int r, int thick, int a0, int a1)
{
    for (int a = a0; a <= a1; a += 2) {
        int x, y;
        polar(cx, cy, a, r, &x, &y);
        if (x - thick < *bx0) *bx0 = x - thick;
        if (y - thick < *by0) *by0 = y - thick;
        if (x + thick > *bx1) *bx1 = x + thick;
        if (y + thick > *by1) *by1 = y + thick;
    }
}

/* 只渲染指定矩形区域内的表盘内容（背景 + 秒环 + 刻度 + 指针） */
static void analog_draw_rect(int rx0, int ry0, int rx1, int ry1, const rtc_time_t *t)
{
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > LCD_H_RES) rx1 = LCD_H_RES;
    if (ry1 > LCD_V_RES) ry1 = LCD_V_RES;
    if (rx0 >= rx1 || ry0 >= ry1) {
        return;
    }

    const int cx = RING_CX, cy = RING_CY;
    const int ro = RING_R, ri = RING_R - RING_W;
    const int w = rx1 - rx0;
    const bool minimal = (s_face == FACE_ANALOG_MIN);

    for (int y = ry0; y < ry1; y += DISPLAY_BAND_LINES) {
        const int lines = ((ry1 - y) < DISPLAY_BAND_LINES) ? (ry1 - y) : DISPLAY_BAND_LINES;
        uint16_t *buf = display_band_buffer();

        for (int yy = 0; yy < lines; yy++) {
            const int sy = y + yy;
            const int dy = sy - cy;
            for (int xx = 0; xx < w; xx++) {
                const int sx = rx0 + xx;
                const int dx = sx - cx;
                const int d2 = dx * dx + dy * dy;
                uint16_t c = COLOR_BLACK;

                if (!minimal && d2 <= ro * ro && d2 >= ri * ri) {
                    float deg = atan2f((float)dx, (float)(-dy)) * 57.29578f;
                    if (deg < 0.0f) {
                        deg += 360.0f;
                    }
                    c = (deg < (float)(t->second * 6)) ? COLOR_GREEN : COLOR_TRACK;
                }
                buf[yy * w + xx] = c;
            }
        }

        /* 刻度（简约盘没有） */
        if (!minimal) {
            for (int i = 0; i < 60; i++) {
                const int big = (i % 5) == 0;
                int x1, y1, x2, y2;
                polar(cx, cy, i * 6, big ? 136 : 148, &x1, &y1);
                polar(cx, cy, i * 6, 156, &x2, &y2);
                buf_line(buf, w, y, lines, rx0, x1, y1, x2, y2, big ? 5 : 2,
                         big ? COLOR_WHITE : COLOR_GRAY);
            }
        }

        /* 指针：分段绘制，做出根部粗、尖端细的效果 */
        int hx, hy, mx, my, sx, sy2, tx, ty2, hx2, hy2, mx2, my2;
        const int hour_deg = (t->hour % 12) * 30 + t->minute / 2;
        const int min_deg = t->minute * 6 + t->second / 10;
        const int sec_deg = t->second * 6;

        polar(cx, cy, hour_deg, ANALOG_HOUR_LEN, &hx, &hy);
        polar(cx, cy, hour_deg, ANALOG_HOUR_LEN * 5 / 9, &hx2, &hy2);
        polar(cx, cy, min_deg, ANALOG_MIN_LEN, &mx, &my);
        polar(cx, cy, min_deg, ANALOG_MIN_LEN * 5 / 9, &mx2, &my2);
        polar(cx, cy, sec_deg, ANALOG_SEC_LEN, &sx, &sy2);
        polar(cx, cy, sec_deg + 180, 24, &tx, &ty2);

        buf_line(buf, w, y, lines, rx0, cx, cy, hx2, hy2, ANALOG_HOUR_TH, COLOR_WHITE);
        buf_line(buf, w, y, lines, rx0, hx2, hy2, hx, hy, ANALOG_HOUR_TH - 2, COLOR_WHITE);

        buf_line(buf, w, y, lines, rx0, cx, cy, mx2, my2, ANALOG_MIN_TH, COLOR_WHITE);
        buf_line(buf, w, y, lines, rx0, mx2, my2, mx, my, ANALOG_MIN_TH - 2, COLOR_WHITE);

        buf_line(buf, w, y, lines, rx0, cx, cy, sx, sy2, ANALOG_SEC_TH, COLOR_RED);
        buf_line(buf, w, y, lines, rx0, cx, cy, tx, ty2, ANALOG_SEC_TH, COLOR_RED);

        /* 中心轴 */
        buf_disc(buf, w, y, lines, rx0, cx, cy, 7, COLOR_WHITE);
        buf_disc(buf, w, y, lines, rx0, cx, cy, 3, COLOR_RED);

        display_band_push(rx0, y, w, lines);
    }
}

static void analog_draw_date(const rtc_time_t *t)
{
    char buf[48];

    if (s_soc >= 0) {
        snprintf(buf, sizeof(buf), "%d月%d日 %s %d%%", t->month, t->day,
                 WEEK_CN[t->weekday % 7], s_soc);
    } else {
        snprintf(buf, sizeof(buf), "%d月%d日 %s", t->month, t->day,
                 WEEK_CN[t->weekday % 7]);
    }

    /* 按文字宽度铺一块黑底当「日期窗」，遮掉穿字的刻度 */
    const int tw = display_text_width_utf8(buf, 1);
    int x0 = (LCD_H_RES - tw) / 2 - 8;
    if (x0 < 0) x0 = 0;
    display_fill_rect(x0, 291, tw + 16, 25, COLOR_BLACK);
    text_center(298, buf, 1, 0xC618);
}

/* 整块重绘 */
static void draw_analog(const rtc_time_t *t)
{
    display_clear(COLOR_BLACK);
    analog_draw_rect(0, 0, LCD_H_RES, LCD_V_RES, t);
    analog_draw_date(t);

    s_analog_prev = *t;
    s_analog_valid = true;
}

/* 每秒只重绘指针扫过的区域 + 新增的秒环楔形 */
static void analog_tick(const rtc_time_t *t)
{
    if (!s_analog_valid || t->second < s_analog_prev.second) {
        draw_analog(t);
        return;
    }

    const int cx = RING_CX, cy = RING_CY;

    /* 1) 秒环新增的扇形（简约盘没有秒环） */
    if (s_face != FACE_ANALOG_MIN && t->second != s_analog_prev.second) {
        int ax0 = LCD_H_RES, ay0 = LCD_V_RES, ax1 = 0, ay1 = 0;
        grow_arc_bbox(&ax0, &ay0, &ax1, &ay1, cx, cy, RING_R, RING_W,
                      s_analog_prev.second * 6, t->second * 6);
        analog_draw_rect(ax0, ay0, ax1, ay1, t);
    }

    /* 2) 指针：旧位置 + 新位置 */
    int bx0 = LCD_H_RES, by0 = LCD_V_RES, bx1 = 0, by1 = 0;

    grow_hand_bbox(&bx0, &by0, &bx1, &by1, cx, cy,
                   s_analog_prev.second * 6, ANALOG_SEC_LEN, ANALOG_SEC_TH + 4);
    grow_hand_bbox(&bx0, &by0, &bx1, &by1, cx, cy,
                   t->second * 6, ANALOG_SEC_LEN, ANALOG_SEC_TH + 4);

    grow_hand_bbox(&bx0, &by0, &bx1, &by1, cx, cy,
                   s_analog_prev.minute * 6 + s_analog_prev.second / 10,
                   ANALOG_MIN_LEN, ANALOG_MIN_TH + 4);
    grow_hand_bbox(&bx0, &by0, &bx1, &by1, cx, cy,
                   t->minute * 6 + t->second / 10, ANALOG_MIN_LEN, ANALOG_MIN_TH + 4);

    grow_hand_bbox(&bx0, &by0, &bx1, &by1, cx, cy,
                   (s_analog_prev.hour % 12) * 30 + s_analog_prev.minute / 2,
                   ANALOG_HOUR_LEN, ANALOG_HOUR_TH + 4);
    grow_hand_bbox(&bx0, &by0, &bx1, &by1, cx, cy,
                   (t->hour % 12) * 30 + t->minute / 2, ANALOG_HOUR_LEN, ANALOG_HOUR_TH + 4);

    analog_draw_rect(bx0, by0, bx1 + 1, by1 + 1, t);

    /* 指针扫过底部会擦掉日期，所以每帧都补画一次（成本很小） */
    analog_draw_date(t);

    s_analog_prev = *t;
}

static int s_ring_prev_sec = -1;

static void classic_text(const rtc_time_t *t, bool with_ampm)
{
    char buf[32];

    if (with_ampm && !s_h24) {
        text_center(112, (t->hour < 12) ? "上午" : "下午", 1, COLOR_GRAY);
    }

    format_time(t, buf, sizeof(buf), s_h24);
    text_center(140, buf, 4, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%d年%d月%d日", t->year, t->month, t->day);
    text_center(232, buf, 1, COLOR_CYAN);

    text_center(258, WEEK_CN[t->weekday % 7], 1, COLOR_GRAY);

    draw_battery_line(310);
}

static void draw_classic(const rtc_time_t *t, bool full)
{
    if (!full) {
        return;
    }

    display_clear(COLOR_BLACK);
    display_draw_ring(RING_CX, RING_CY, RING_R, RING_W, 0, t->second * 6,
                      COLOR_GREEN, COLOR_TRACK, COLOR_BLACK);
    s_ring_prev_sec = t->second;
    classic_text(t, true);
}

/* 秒环增量刷新；文字每秒重画一次，保证任何时候都是完整的 */
static void classic_tick(const rtc_time_t *t, bool minute_changed, bool day_changed)
{
    (void)minute_changed;
    (void)day_changed;

    const bool need_full_ring = (s_ring_prev_sec < 0) || (t->second < s_ring_prev_sec) ||
                                (t->second - s_ring_prev_sec > 10);

    if (need_full_ring) {
        display_draw_ring(RING_CX, RING_CY, RING_R, RING_W, 0, t->second * 6,
                          COLOR_GREEN, COLOR_TRACK, COLOR_BLACK);
    } else if (t->second != s_ring_prev_sec) {
        display_draw_arc(RING_CX, RING_CY, RING_R, RING_W,
                         s_ring_prev_sec * 6, t->second * 6,
                         COLOR_GREEN, COLOR_TRACK, COLOR_BLACK);
    }
    s_ring_prev_sec = t->second;

    /* 文字每秒重画：成本小（约 25KB），但能保证不会长时间停留在缺损状态 */
    classic_text(t, true);
}

static void draw_minimal(const rtc_time_t *t, bool full)
{
    char buf[32];

    if (full) {
        display_clear(COLOR_BLACK);

        snprintf(buf, sizeof(buf), "%d年%d月%d日 %s", t->year, t->month, t->day,
                 WEEK_CN[t->weekday % 7]);
        text_center(36, buf, 1, COLOR_GRAY);

        format_time(t, buf, sizeof(buf), s_h24);
        text_center(112, buf, 6, COLOR_WHITE);

        snprintf(buf, sizeof(buf), "%d 秒", t->second);
        text_center(232, buf, 2, COLOR_GREEN);

        draw_battery_line(300);
    }
}

static void draw_info_row(int y, const char *text)
{
    display_fill_rect(0, y, LCD_H_RES, 22, COLOR_BLACK);
    display_draw_text_utf8(24, y, text, 1, COLOR_WHITE, COLOR_BLACK);
}

static void draw_info(const rtc_time_t *t, bool full)
{
    char buf[64];

    if (full) {
        display_clear(COLOR_BLACK);
        text_center(28, "设备信息", 2, COLOR_CYAN);
    }

    snprintf(buf, sizeof(buf), "电量   %s", (s_soc < 0) ? "--" : "");
    if (s_soc >= 0) {
        snprintf(buf, sizeof(buf), "电量   %d%%", s_soc);
    }
    draw_info_row(96, buf);

    if (s_mv > 0) {
        snprintf(buf, sizeof(buf), "电压   %d.%02d V", s_mv / 1000, (s_mv % 1000) / 10);
    } else {
        snprintf(buf, sizeof(buf), "电压   --");
    }
    draw_info_row(134, buf);

    snprintf(buf, sizeof(buf), "时间   %02d:%02d:%02d", t->hour, t->minute, t->second);
    draw_info_row(172, buf);

    snprintf(buf, sizeof(buf), "日期   %04d-%02d-%02d %s", t->year, t->month, t->day,
             WEEK_CN[t->weekday % 7]);
    draw_info_row(210, buf);

    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf), "运行   %02lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60), (unsigned long)(up % 60));
    draw_info_row(248, buf);

    snprintf(buf, sizeof(buf), "表盘   %s", FACE_NAME[s_face]);
    draw_info_row(286, buf);
}

/* ---- 天气表盘 ---- */

/* 64x64 天气图标：晴/月、云、雨、雪 */
static void draw_weather_icon(int cx, int cy, int code, bool is_day)
{
    const int S = 64;
    const int x0 = cx - S / 2;
    const int y0 = cy - S / 2;

    const bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
    const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    const bool cloudy = (code >= 2) || rain || snow;

    for (int yy = 0; yy < S; yy += DISPLAY_BAND_LINES) {
        const int lines = ((S - yy) < DISPLAY_BAND_LINES) ? (S - yy) : DISPLAY_BAND_LINES;
        uint16_t *buf = display_band_buffer();
        const int by = y0 + yy;

        for (int i = 0; i < S * lines; i++) {
            buf[i] = COLOR_BLACK;
        }

        if (!cloudy) {
            if (is_day) {
                buf_disc(buf, S, by, lines, x0, cx, cy, 11, 0xFE60);
                for (int a = 0; a < 360; a += 45) {
                    int x1, y1, x2, y2;
                    polar(cx, cy, a, 16, &x1, &y1);
                    polar(cx, cy, a, 24, &x2, &y2);
                    buf_line(buf, S, by, lines, x0, x1, y1, x2, y2, 3, 0xFE60);
                }
            } else {
                buf_disc(buf, S, by, lines, x0, cx, cy, 16, 0xFFE0);
                buf_disc(buf, S, by, lines, x0, cx + 9, cy - 6, 14, COLOR_BLACK);
            }
        } else {
            buf_disc(buf, S, by, lines, x0, cx - 14, cy - 2, 12, 0xE71C);
            buf_disc(buf, S, by, lines, x0, cx + 2, cy - 10, 15, 0xE71C);
            buf_disc(buf, S, by, lines, x0, cx + 16, cy - 2, 11, 0xE71C);
            buf_line(buf, S, by, lines, x0, cx - 14, cy + 7, cx + 16, cy + 7, 9, 0xE71C);

            if (rain) {
                for (int i = -1; i <= 1; i++) {
                    buf_line(buf, S, by, lines, x0, cx + i * 10, cy + 14,
                             cx + i * 10 - 3, cy + 24, 3, COLOR_CYAN);
                }
            } else if (snow) {
                buf_disc(buf, S, by, lines, x0, cx - 10, cy + 18, 3, COLOR_WHITE);
                buf_disc(buf, S, by, lines, x0, cx, cy + 22, 3, COLOR_WHITE);
                buf_disc(buf, S, by, lines, x0, cx + 10, cy + 18, 3, COLOR_WHITE);
            }
        }

        display_band_push(x0, by, S, lines);
    }
}

static void draw_weather(const rtc_time_t *t)
{
    weather_info_t w;
    char buf[64];

    weather_get(&w);
    display_clear(COLOR_BLACK);

    /* 顶部：城市 + 时间 */
    format_time(t, buf, sizeof(buf), true);
    char top[96];
    snprintf(top, sizeof(top), "%s  %s", WEATHER_CITY, buf);
    text_center(20, top, 1, COLOR_GRAY);

    /* 图标 + 温度 */
    if (w.valid) {
        draw_weather_icon(108, 104, w.code, w.is_day);
        snprintf(buf, sizeof(buf), "%d℃", w.temp_c);
    } else {
        snprintf(buf, sizeof(buf), "--℃");
    }
    display_draw_text_utf8(194, 78, buf, 4, COLOR_WHITE, COLOR_BLACK);

    /* 天气描述 */
    text_center(172, w.valid ? weather_text(w.code) : "获取中...", 2, COLOR_CYAN);

    /* 今日高低温 */
    if (w.valid) {
        snprintf(buf, sizeof(buf), "最高 %d℃   最低 %d℃", w.tmax_c, w.tmin_c);
    } else {
        snprintf(buf, sizeof(buf), "最高 --   最低 --");
    }
    text_center(214, buf, 1, COLOR_GRAY);

    /* 湿度 + 风 */
    if (w.valid) {
        snprintf(buf, sizeof(buf), "湿度 %d%%   风 %dkm/h", w.humidity, w.wind_kmh);
    } else {
        snprintf(buf, sizeof(buf), "湿度 --   风 --");
    }
    text_center(242, buf, 1, COLOR_GRAY);

    /* 日期 */
    snprintf(buf, sizeof(buf), "%d月%d日 %s", t->month, t->day, WEEK_CN[t->weekday % 7]);
    text_center(300, buf, 1, COLOR_GRAY);
}

static void render_full(const rtc_time_t *t)
{
    switch (s_face) {
    case FACE_ANALOG:
    case FACE_ANALOG_MIN:
        draw_analog(t);
        break;
    case FACE_CLASSIC:
        draw_classic(t, true);
        break;
    case FACE_MINIMAL:
        draw_minimal(t, true);
        break;
    case FACE_INFO:
        draw_info(t, true);
        break;
    case FACE_WEATHER:
        draw_weather(t);
        break;
    default:
        break;
    }
}

static void render_tick(const rtc_time_t *t, bool minute_changed, bool day_changed)
{
    char buf[32];

    switch (s_face) {
    case FACE_ANALOG:
    case FACE_ANALOG_MIN:
        analog_tick(t);
        break;
    case FACE_CLASSIC:
        classic_tick(t, minute_changed, day_changed);
        break;

    case FACE_MINIMAL:
        display_fill_rect(0, 228, LCD_H_RES, 40, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d 秒", t->second);
        text_center(232, buf, 2, COLOR_GREEN);
        if (day_changed) {
            display_fill_rect(0, 32, LCD_H_RES, 24, COLOR_BLACK);
            snprintf(buf, sizeof(buf), "%d年%d月%d日 %s", t->year, t->month, t->day,
                     WEEK_CN[t->weekday % 7]);
            text_center(36, buf, 1, COLOR_GRAY);
        }
        break;

    case FACE_INFO:
        draw_info(t, false);
        break;

    case FACE_WEATHER:
        if (minute_changed) {
            draw_weather(t);
        }
        break;

    default:
        break;
    }
}

/* ---------------- 任务 ---------------- */

static void watch_task(void *arg)
{
    (void)arg;

    touch_point_t pt;
    bool pressed_prev = false;
    bool pressed_now = false;
    uint16_t start_x = 0;
    int max_dx = 0;
    int64_t press_start_us = 0;
    int64_t last_input_us = esp_timer_get_time();
    bool dimmed = false;
    uint32_t last_weather_seq = 0;

    const bool rtc_ok = pcf85063_present();
    (void)rtc_ok;
    rtc_time_t now;
    int last_second = -1;
    int last_minute = -1;
    int last_day = -1;
    int64_t last_batt_us = 0;

    /* 软件时钟：先用编译时间兜底，SNTP 校时成功后由 watch_set_time() 校准 */
    clock_from_build_time();

    if (bq27220_present()) {
        bq27220_read(&s_soc, &s_mv);
    }

    for (;;) {
        clock_from_software(&now);

        /* 电量每 5 秒刷新一次 */
        const int64_t now_us = esp_timer_get_time();
        if (bq27220_present() && (now_us - last_batt_us > 5000000)) {
            last_batt_us = now_us;
            bq27220_read(&s_soc, &s_mv);
            if (s_face == FACE_CLASSIC) {
                draw_battery_line(310);
            } else if (s_face == FACE_MINIMAL) {
                draw_battery_line(300);
            }
        }

        const bool sec_changed = (now.second != last_second);
        const bool min_changed = (now.minute != last_minute);
        const bool day_changed = (now.day != last_day);

        if (sec_changed || min_changed || day_changed) {
            render_tick(&now, min_changed, day_changed);
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
        if (s_face == FACE_WEATHER && wseq != last_weather_seq) {
            last_weather_seq = wseq;
            draw_weather(&now);
        }

        /* 触摸：
         *   轻触          -> 下一个表盘
         *   左右滑动      -> 上/下一个表盘
         *   长按 >1 秒    -> 切换 12/24 小时制
         */
        pressed_now = touch_read(&pt);
        if (pressed_now) {
            if (!pressed_prev) {
                start_x = pt.x;
                max_dx = 0;
                press_start_us = esp_timer_get_time();
            }
            const int dx = (int)pt.x - (int)start_x;
            if (abs(dx) > abs(max_dx)) {
                max_dx = dx;
            }
        } else if (pressed_prev) {
            const int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;

            if (held_ms > 1000) {
                if (s_face == FACE_WEATHER) {
                    /* 天气盘上长按 = 立即刷新 */
                    net_request_refresh();
                    ESP_LOGI(TAG, "weather refresh requested");
                } else {
                    s_h24 = !s_h24;
                    ESP_LOGI(TAG, "long press -> %s", s_h24 ? "24h" : "12h");
                    render_full(&now);
                }
            } else if (max_dx >= SWIPE_THRESHOLD) {
                s_face = (face_t)((s_face + FACE_COUNT - 1) % FACE_COUNT);
                ESP_LOGI(TAG, "swipe right -> face %s", FACE_NAME[s_face]);
                render_full(&now);
            } else {
                /* 左滑和轻触都切到下一个表盘 */
                s_face = (face_t)((s_face + 1) % FACE_COUNT);
                ESP_LOGI(TAG, "%s -> face %s", (max_dx <= -SWIPE_THRESHOLD) ? "swipe left" : "tap",
                         FACE_NAME[s_face]);
                render_full(&now);
            }
        }
        pressed_prev = pressed_now;

        /* 30 秒无操作 → 背光降到 15%，触摸恢复 */
        if (pressed_now) {
            last_input_us = esp_timer_get_time();
            if (dimmed) {
                dimmed = false;
                display_set_backlight(100);
                ESP_LOGI(TAG, "backlight: on");
            }
        } else if (!dimmed && (esp_timer_get_time() - last_input_us > 30000000LL)) {
            dimmed = true;
            display_set_backlight(15);
            ESP_LOGI(TAG, "backlight: dimmed");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void watch_app_start(void)
{
    display_clear(COLOR_BLACK);
    text_center(160, "启动中...", 2, COLOR_GRAY);

    xTaskCreate(watch_task, "watch", 6144, NULL, 5, NULL);
}
