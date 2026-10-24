/*
 * UI 共享层实现：全局状态、文本工具、行缓冲矢量绘制。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ui.h"

#include <math.h>
#include <stdio.h>

const char *const ui_face_name[FACE_COUNT] = {
    "指针", "简约", "经典", "极简", "信息", "天气", "贪吃蛇", "步数",
};

const char *const ui_week_cn[7] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
};

face_t ui_face = FACE_CLASSIC;
bool ui_h24 = true;
int ui_soc = -1;
int ui_mv = -1;

void ui_text_center(int y, const char *text, uint8_t scale, uint16_t fg)
{
    display_draw_text_utf8((LCD_H_RES - display_text_width_utf8(text, scale)) / 2,
                           y, text, scale, fg, COLOR_BLACK);
}

void ui_format_time(const rtc_time_t *t, char *buf, size_t len, bool h24)
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

void ui_draw_battery_line(int y)
{
    char buf[32];

    if (ui_soc < 0) {
        snprintf(buf, sizeof(buf), "电池 --");
    } else {
        snprintf(buf, sizeof(buf), "电池 %d%%", ui_soc);
    }

    display_fill_rect(0, y - 6, LCD_H_RES, 22, COLOR_BLACK);
    ui_text_center(y, buf, 1, COLOR_GRAY);
}

/* ---------------- 行缓冲矢量绘制 ---------------- */

void ui_px(uint16_t *buf, int w, int y0, int lines, int rx0, int x, int y, uint16_t c)
{
    const int row = y - y0;
    const int col = x - rx0;
    if (col < 0 || col >= w || row < 0 || row >= lines) {
        return;
    }
    buf[row * w + col] = c;
}

void ui_disc(uint16_t *buf, int w, int y0, int lines, int rx0,
             int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                ui_px(buf, w, y0, lines, rx0, cx + dx, cy + dy, c);
            }
        }
    }
}

void ui_line(uint16_t *buf, int w, int y0, int lines, int rx0,
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
        ui_disc(buf, w, y0, lines, rx0, x1, y1, thick / 2, c);
        return;
    }

    const int r = thick / 2;
    for (int i = 0; i <= steps; i++) {
        const int x = x1 + dx * i / steps;
        const int y = y1 + dy * i / steps;
        if (r <= 0) {
            ui_px(buf, w, y0, lines, rx0, x, y, c);
        } else {
            ui_disc(buf, w, y0, lines, rx0, x, y, r, c);
        }
    }
}

void ui_polar(int cx, int cy, int deg, int r, int *x, int *y)
{
    const float a = (float)deg * 3.14159265f / 180.0f;
    *x = cx + (int)(sinf(a) * (float)r);
    *y = cy - (int)(cosf(a) * (float)r);
}
