/*
 * 涓绘満绔覆鏌撻獙璇侊細鐢ㄦ々鍑芥暟鏇挎崲 esp_lcd / esp_timer / esp_random锛? * 璺?anim.c 鐨勭湡瀹炵墿鐞嗕笌缁樺埗浠ｇ爜锛屾妸缁撴灉瀵兼垚 PPM 妫€鏌ョ敾闈€? */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "display.h"
#include "imu.h"
#include "ui.h"

/* 模拟倾斜：0 = 平放，1 = 向右下倾斜（y 轴出现 -0.35g 分量） */
static int g_tilt;
static int g_shake;
static int g_shake_toggle;

esp_err_t imu_read_accel(float *ax, float *ay, float *az)
{
    if (g_shake) {
        /* 模拟抖腕：x 轴 ±1.6g 来回摆 */
        g_shake_toggle ^= 1;
        *ax = g_shake_toggle ? 1.6f : -1.6f;
        *ay = 0.0f;
        *az = 0.9f;
        return ESP_OK;
    }

    *ax = 0.0f;
    *ay = g_tilt ? -0.35f : 0.0f;
    *az = g_tilt ? 0.94f : 1.0f;
    return ESP_OK;
}

static uint16_t g_fb[LCD_H_RES * LCD_V_RES];
static uint16_t g_band[LCD_H_RES * DISPLAY_BAND_LINES];
static int64_t g_now_us;
static uint32_t g_rng = 0x12345678u;
static long g_push_px;

uint32_t esp_random(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

void display_frame_begin(void) {}
void display_frame_end(void) {}

void display_clear(uint16_t c)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        g_fb[i] = c;
    }
}

void display_draw_ring(int cx, int cy, int radius, int thickness,
                       int start_deg, int end_deg,
                       uint16_t fg, uint16_t track, uint16_t bg)
{
    (void)start_deg;
    (void)end_deg;
    (void)fg;
    const int r_in = radius - thickness;

    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) {
                continue;
            }
            const int dx = x - cx;
            const int dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= radius * radius && d2 >= r_in * r_in) {
                g_fb[y * LCD_H_RES + x] = track;
            }
        }
    }
    (void)bg;
}

uint16_t *display_band_buffer(void)
{
    return g_band;
}

void display_band_push(int x0, int y, int w, int lines)
{
    g_push_px += (long)w * lines;
    for (int r = 0; r < lines; r++) {
        memcpy(&g_fb[(y + r) * LCD_H_RES + x0], &g_band[r * w], (size_t)w * sizeof(uint16_t));
    }
}

void ui_disc(uint16_t *buf, int w, int y0, int lines, int rx0,
             int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            const int x = cx + dx;
            const int y = cy + dy;
            const int row = y - y0;
            const int col = x - rx0;
            if (col >= 0 && col < w && row >= 0 && row < lines) {
                buf[row * w + col] = c;
            }
        }
    }
}

void ui_px(uint16_t *buf, int w, int y0, int lines, int rx0, int x, int y, uint16_t c)
{
    const int row = y - y0;
    const int col = x - rx0;
    if (col < 0 || col >= w || row < 0 || row >= lines) {
        return;
    }
    buf[row * w + col] = c;
}

void ui_polar(int cx, int cy, int deg, int r, int *x, int *y)
{
    const float a = (float)deg * 3.14159265f / 180.0f;
    *x = cx + (int)(sinf(a) * (float)r);
    *y = cy - (int)(cosf(a) * (float)r);
}

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", LCD_H_RES, LCD_V_RES);
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        const uint16_t c = g_fb[i];
        const unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void stats(const char *tag)
{
    int non_black = 0;
    int max_d2 = 0;
    int minx = 999, maxx = -1, miny = 999, maxy = -1;
    int top = 0, bottom = 0;
    int ring = 0;

    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            const uint16_t c = g_fb[y * LCD_H_RES + x];
            if (c == COLOR_BLACK) {
                continue;
            }
            if (c == 0x2104) {
                ring++;
                continue;
            }
            if (y < LCD_V_RES / 2) {
                top++;
            } else {
                bottom++;
            }
            non_black++;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            const int dx = x - 180;
            const int dy = y - 180;
            if (dx * dx + dy * dy > max_d2) {
                max_d2 = dx * dx + dy * dy;
            }
        }
    }
    printf("%-12s balls=%5d ring=%5d top=%5d bottom=%5d bbox=(%3d,%3d)-(%3d,%3d) max_r=%d\n",
           tag, non_black, ring, top, bottom, minx, miny, maxx, maxy, (int)sqrtf((float)max_d2));
}

int main(int argc, char **argv)
{
    const int frames = (argc > 1) ? atoi(argv[1]) : 600;

    g_now_us = 0;
    anim_start();
    dump_ppm("hosttest/frame_0000.ppm");
    stats("frame 0");

    for (int f = 1; f <= frames; f++) {
        g_now_us += 33000;
        anim_poll();

        if (f == 400) {
            g_tilt = 1;
            printf("tilt ON at frame %d\n", f);
        }
        if (f == 600) {
            g_shake = 1;
            printf("SHAKE at frame %d\n", f);
        }
        if (f == 620) {
            g_shake = 0;
        }
        if (f == 900) {
            g_tilt = 0;
            printf("tilt OFF at frame %d\n", f);
        }
        if (f % 250 == 0) {
            char tag[32];
            snprintf(tag, sizeof(tag), "f%d", f);
            stats(tag);
        }
        if (f == 1) {
            dump_ppm("hosttest/frame_0001.ppm");
            stats("frame 1");
        }
        if (f == 30) {
            dump_ppm("hosttest/frame_0030.ppm");
            stats("frame 30");
        }
        if (f == 150) {
            dump_ppm("hosttest/frame_0150.ppm");
            stats("frame 150");
        }
        if (f == 300) {
            anim_kick(180, 180);
            printf("kick at frame %d\n", f);
        }
        if (f == 305) {
            dump_ppm("hosttest/frame_0305.ppm");
            stats("frame 305");
        }
        if (f == frames) {
            dump_ppm("hosttest/frame_last.ppm");
            stats("frame last");
        }
    }

    printf("avg push: %ld px/frame = %.1f KB/frame = %.0f KB/s @30fps\n",
           g_push_px / (frames + 1), (double)g_push_px / (frames + 1) * 2.0 / 1024.0,
           (double)g_push_px / (frames + 1) * 2.0 / 1024.0 * 30.0);
    return 0;
}

