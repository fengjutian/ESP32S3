/*
 * 六种表盘的实现。
 *
 *   指针 / 简约  —— 模拟指针，按脏区域增量刷新
 *   经典 / 极简  —— 数字时间
 *   信息         —— 电量、电压、运行时长
 *   天气         —— Open-Meteo 数据 + 程序化图标
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "faces.h"

#include <math.h>
#include <stdio.h>

#include "esp_timer.h"

#include "app_config.h"
#include "display.h"
#include "snake.h"
#include "ui.h"
#include "weather.h"

/* ================= 指针表盘 ================= */

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
    ui_polar(cx, cy, deg, len, &tx, &ty);

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
        ui_polar(cx, cy, a, r, &x, &y);
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

    const int cx = UI_RING_CX;
    const int cy = UI_RING_CY;
    const int ro = UI_RING_R;
    const int ri = UI_RING_R - UI_RING_W;
    const int w = rx1 - rx0;
    const bool minimal = (ui_face == FACE_ANALOG_MIN);

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
                    c = (deg < (float)(t->second * 6)) ? COLOR_GREEN : UI_COLOR_TRACK;
                }
                buf[yy * w + xx] = c;
            }
        }

        /* 刻度（简约盘没有） */
        if (!minimal) {
            for (int i = 0; i < 60; i++) {
                const int big = (i % 5) == 0;
                int x1, y1, x2, y2;
                ui_polar(cx, cy, i * 6, big ? 136 : 148, &x1, &y1);
                ui_polar(cx, cy, i * 6, 156, &x2, &y2);
                ui_line(buf, w, y, lines, rx0, x1, y1, x2, y2, big ? 5 : 2,
                        big ? COLOR_WHITE : COLOR_GRAY);
            }
        }

        /* 指针：分两段绘制，根部粗、尖端细 */
        int hx, hy, mx, my, sx, sy, tx, ty, hx2, hy2, mx2, my2;
        const int hour_deg = (t->hour % 12) * 30 + t->minute / 2;
        const int min_deg = t->minute * 6 + t->second / 10;
        const int sec_deg = t->second * 6;

        ui_polar(cx, cy, hour_deg, ANALOG_HOUR_LEN, &hx, &hy);
        ui_polar(cx, cy, hour_deg, ANALOG_HOUR_LEN * 5 / 9, &hx2, &hy2);
        ui_polar(cx, cy, min_deg, ANALOG_MIN_LEN, &mx, &my);
        ui_polar(cx, cy, min_deg, ANALOG_MIN_LEN * 5 / 9, &mx2, &my2);
        ui_polar(cx, cy, sec_deg, ANALOG_SEC_LEN, &sx, &sy);
        ui_polar(cx, cy, sec_deg + 180, 24, &tx, &ty);

        ui_line(buf, w, y, lines, rx0, cx, cy, hx2, hy2, ANALOG_HOUR_TH, COLOR_WHITE);
        ui_line(buf, w, y, lines, rx0, hx2, hy2, hx, hy, ANALOG_HOUR_TH - 2, COLOR_WHITE);

        ui_line(buf, w, y, lines, rx0, cx, cy, mx2, my2, ANALOG_MIN_TH, COLOR_WHITE);
        ui_line(buf, w, y, lines, rx0, mx2, my2, mx, my, ANALOG_MIN_TH - 2, COLOR_WHITE);

        ui_line(buf, w, y, lines, rx0, cx, cy, sx, sy, ANALOG_SEC_TH, COLOR_RED);
        ui_line(buf, w, y, lines, rx0, cx, cy, tx, ty, ANALOG_SEC_TH, COLOR_RED);

        /* 中心轴 */
        ui_disc(buf, w, y, lines, rx0, cx, cy, 7, COLOR_WHITE);
        ui_disc(buf, w, y, lines, rx0, cx, cy, 3, COLOR_RED);

        display_band_push(rx0, y, w, lines);
    }
}

/* 底部日期窗：按文字宽度铺黑底，遮掉穿字的刻度 */
static void analog_draw_date(const rtc_time_t *t)
{
    char buf[48];

    if (ui_soc >= 0) {
        snprintf(buf, sizeof(buf), "%d月%d日 %s %d%%", t->month, t->day,
                 ui_week_cn[t->weekday % 7], ui_soc);
    } else {
        snprintf(buf, sizeof(buf), "%d月%d日 %s", t->month, t->day,
                 ui_week_cn[t->weekday % 7]);
    }

    const int tw = display_text_width_utf8(buf, 1);
    int x0 = (LCD_H_RES - tw) / 2 - 8;
    if (x0 < 0) x0 = 0;
    display_fill_rect(x0, 291, tw + 16, 25, COLOR_BLACK);
    ui_text_center(298, buf, 1, 0xC618);
}

static void analog_render(const rtc_time_t *t)
{
    display_clear(COLOR_BLACK);
    analog_draw_rect(0, 0, LCD_H_RES, LCD_V_RES, t);
    analog_draw_date(t);

    s_analog_prev = *t;
    s_analog_valid = true;
}

/* 每秒只重绘指针扫过的区域 + 新增的秒环扇形 */
static void analog_tick(const rtc_time_t *t)
{
    if (!s_analog_valid || t->second < s_analog_prev.second) {
        analog_render(t);
        return;
    }

    const int cx = UI_RING_CX, cy = UI_RING_CY;

    if (ui_face != FACE_ANALOG_MIN && t->second != s_analog_prev.second) {
        int ax0 = LCD_H_RES, ay0 = LCD_V_RES, ax1 = 0, ay1 = 0;
        grow_arc_bbox(&ax0, &ay0, &ax1, &ay1, cx, cy, UI_RING_R, UI_RING_W,
                      s_analog_prev.second * 6, t->second * 6);
        analog_draw_rect(ax0, ay0, ax1, ay1, t);
    }

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

    /* 指针扫过底部会擦掉日期，每帧补画 */
    analog_draw_date(t);

    s_analog_prev = *t;
}

/* ================= 经典表盘 ================= */

static int s_ring_prev_sec = -1;

static void classic_text(const rtc_time_t *t, bool with_ampm)
{
    char buf[32];

    if (with_ampm && !ui_h24) {
        ui_text_center(112, (t->hour < 12) ? "上午" : "下午", 1, COLOR_GRAY);
    }

    ui_format_time(t, buf, sizeof(buf), ui_h24);
    ui_text_center(140, buf, 4, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%d年%d月%d日", t->year, t->month, t->day);
    ui_text_center(232, buf, 1, COLOR_CYAN);

    ui_text_center(258, ui_week_cn[t->weekday % 7], 1, COLOR_GRAY);

    ui_draw_battery_line(310);
}

static void classic_render(const rtc_time_t *t)
{
    display_clear(COLOR_BLACK);
    display_draw_ring(UI_RING_CX, UI_RING_CY, UI_RING_R, UI_RING_W, 0, t->second * 6,
                      COLOR_GREEN, UI_COLOR_TRACK, COLOR_BLACK);
    s_ring_prev_sec = t->second;
    classic_text(t, true);
}

/* 秒环增量刷新；文字每秒重画一次，保证不会长时间停留缺损状态 */
static void classic_tick(const rtc_time_t *t)
{
    const bool need_full_ring = (s_ring_prev_sec < 0) || (t->second < s_ring_prev_sec) ||
                                (t->second - s_ring_prev_sec > 10);

    if (need_full_ring) {
        display_draw_ring(UI_RING_CX, UI_RING_CY, UI_RING_R, UI_RING_W, 0, t->second * 6,
                          COLOR_GREEN, UI_COLOR_TRACK, COLOR_BLACK);
    } else if (t->second != s_ring_prev_sec) {
        display_draw_arc(UI_RING_CX, UI_RING_CY, UI_RING_R, UI_RING_W,
                         s_ring_prev_sec * 6, t->second * 6,
                         COLOR_GREEN, UI_COLOR_TRACK, COLOR_BLACK);
    }
    s_ring_prev_sec = t->second;

    classic_text(t, true);
}

/* ================= 极简表盘（超大数字） ================= */

static void minimal_render(const rtc_time_t *t)
{
    char buf[40];

    display_clear(COLOR_BLACK);

    snprintf(buf, sizeof(buf), "%d年%d月%d日 %s", t->year, t->month, t->day,
             ui_week_cn[t->weekday % 7]);
    ui_text_center(36, buf, 1, COLOR_GRAY);

    ui_format_time(t, buf, sizeof(buf), ui_h24);
    ui_text_center(112, buf, 6, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%d 秒", t->second);
    ui_text_center(232, buf, 2, COLOR_GREEN);

    ui_draw_battery_line(300);
}

static void minimal_tick(const rtc_time_t *t, bool day_changed)
{
    char buf[24];

    display_fill_rect(0, 228, LCD_H_RES, 40, COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%d 秒", t->second);
    ui_text_center(232, buf, 2, COLOR_GREEN);

    if (day_changed) {
        display_fill_rect(0, 32, LCD_H_RES, 24, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d年%d月%d日 %s", t->year, t->month, t->day,
                 ui_week_cn[t->weekday % 7]);
        ui_text_center(36, buf, 1, COLOR_GRAY);
    }
}

/* ================= 信息表盘 ================= */

static void info_row(int y, const char *text)
{
    display_fill_rect(0, y, LCD_H_RES, 22, COLOR_BLACK);
    display_draw_text_utf8(24, y, text, 1, COLOR_WHITE, COLOR_BLACK);
}

static void info_render(const rtc_time_t *t, bool full)
{
    char buf[64];

    if (full) {
        display_clear(COLOR_BLACK);
        ui_text_center(28, "设备信息", 2, COLOR_CYAN);
    }

    if (ui_soc >= 0) {
        snprintf(buf, sizeof(buf), "电量   %d%%", ui_soc);
    } else {
        snprintf(buf, sizeof(buf), "电量   --");
    }
    info_row(96, buf);

    if (ui_mv > 0) {
        snprintf(buf, sizeof(buf), "电压   %d.%02d V", ui_mv / 1000, (ui_mv % 1000) / 10);
    } else {
        snprintf(buf, sizeof(buf), "电压   --");
    }
    info_row(134, buf);

    snprintf(buf, sizeof(buf), "时间   %02d:%02d:%02d", t->hour, t->minute, t->second);
    info_row(172, buf);

    snprintf(buf, sizeof(buf), "日期   %04d-%02d-%02d %s", t->year, t->month, t->day,
             ui_week_cn[t->weekday % 7]);
    info_row(210, buf);

    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf), "运行   %02lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
             (unsigned long)(up % 60));
    info_row(248, buf);

    snprintf(buf, sizeof(buf), "表盘   %s", ui_face_name[ui_face]);
    info_row(286, buf);
}

/* ================= 天气表盘 ================= */

/* 64x64 程序化图标：晴 / 月 / 云 / 雨 / 雪 */
static void weather_icon(int cx, int cy, int code, bool is_day)
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
                ui_disc(buf, S, by, lines, x0, cx, cy, 11, 0xFE60);
                for (int a = 0; a < 360; a += 45) {
                    int x1, y1, x2, y2;
                    ui_polar(cx, cy, a, 16, &x1, &y1);
                    ui_polar(cx, cy, a, 24, &x2, &y2);
                    ui_line(buf, S, by, lines, x0, x1, y1, x2, y2, 3, 0xFE60);
                }
            } else {
                ui_disc(buf, S, by, lines, x0, cx, cy, 16, 0xFFE0);
                ui_disc(buf, S, by, lines, x0, cx + 9, cy - 6, 14, COLOR_BLACK);
            }
        } else {
            ui_disc(buf, S, by, lines, x0, cx - 14, cy - 2, 12, 0xE71C);
            ui_disc(buf, S, by, lines, x0, cx + 2, cy - 10, 15, 0xE71C);
            ui_disc(buf, S, by, lines, x0, cx + 16, cy - 2, 11, 0xE71C);
            ui_line(buf, S, by, lines, x0, cx - 14, cy + 7, cx + 16, cy + 7, 9, 0xE71C);

            if (rain) {
                for (int i = -1; i <= 1; i++) {
                    ui_line(buf, S, by, lines, x0, cx + i * 10, cy + 14,
                            cx + i * 10 - 3, cy + 24, 3, COLOR_CYAN);
                }
            } else if (snow) {
                ui_disc(buf, S, by, lines, x0, cx - 10, cy + 18, 3, COLOR_WHITE);
                ui_disc(buf, S, by, lines, x0, cx, cy + 22, 3, COLOR_WHITE);
                ui_disc(buf, S, by, lines, x0, cx + 10, cy + 18, 3, COLOR_WHITE);
            }
        }

        display_band_push(x0, by, S, lines);
    }
}

static void weather_render(const rtc_time_t *t)
{
    weather_info_t w;
    char buf[64];

    weather_get(&w);
    display_clear(COLOR_BLACK);

    /* 顶部：城市 + 时间 */
    char top[96];
    ui_format_time(t, buf, sizeof(buf), true);
    snprintf(top, sizeof(top), "%s  %s", WEATHER_CITY, buf);
    ui_text_center(20, top, 1, COLOR_GRAY);

    /* 图标 + 温度 */
    if (w.valid) {
        weather_icon(108, 104, w.code, w.is_day);
        snprintf(buf, sizeof(buf), "%d℃", w.temp_c);
    } else {
        snprintf(buf, sizeof(buf), "--℃");
    }
    display_draw_text_utf8(194, 78, buf, 4, COLOR_WHITE, COLOR_BLACK);

    /* 描述 */
    ui_text_center(172, w.valid ? weather_text(w.code) : "获取中...", 2, COLOR_CYAN);

    /* 今日高低温 */
    if (w.valid) {
        snprintf(buf, sizeof(buf), "最高 %d℃   最低 %d℃", w.tmax_c, w.tmin_c);
    } else {
        snprintf(buf, sizeof(buf), "最高 --   最低 --");
    }
    ui_text_center(214, buf, 1, COLOR_GRAY);

    /* 湿度 + 风 */
    if (w.valid) {
        snprintf(buf, sizeof(buf), "湿度 %d%%   风 %dkm/h", w.humidity, w.wind_kmh);
    } else {
        snprintf(buf, sizeof(buf), "湿度 --   风 --");
    }
    ui_text_center(242, buf, 1, COLOR_GRAY);

    /* 日期 */
    snprintf(buf, sizeof(buf), "%d月%d日 %s", t->month, t->day, ui_week_cn[t->weekday % 7]);
    ui_text_center(300, buf, 1, COLOR_GRAY);
}

/* ================= 分发 ================= */

void face_render(face_t face, const rtc_time_t *t, bool full)
{
    (void)full;

    switch (face) {
    case FACE_ANALOG:
    case FACE_ANALOG_MIN:
        analog_render(t);
        break;
    case FACE_CLASSIC:
        classic_render(t);
        break;
    case FACE_MINIMAL:
        minimal_render(t);
        break;
    case FACE_INFO:
        info_render(t, true);
        break;
    case FACE_WEATHER:
        weather_render(t);
        break;
    case FACE_SNAKE:
        snake_start();
        break;
    default:
        break;
    }
}

void face_tick(face_t face, const rtc_time_t *t, bool minute_changed, bool day_changed)
{
    switch (face) {
    case FACE_ANALOG:
    case FACE_ANALOG_MIN:
        analog_tick(t);
        break;
    case FACE_CLASSIC:
        classic_tick(t);
        break;
    case FACE_MINIMAL:
        minimal_tick(t, day_changed);
        break;
    case FACE_INFO:
        info_render(t, false);
        break;
    case FACE_WEATHER:
        if (minute_changed) {
            weather_render(t);
        }
        break;
    default:
        break;
    }
}
