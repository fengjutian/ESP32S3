#pragma once
#include <stdbool.h>
#include <stdint.h>

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_GRAY    0x8410
#define COLOR_ORANGE  0xFD20

#define LCD_H_RES 360
#define LCD_V_RES 360
#define DISPLAY_BAND_LINES 16

void display_clear(uint16_t color);
void display_draw_ring(int cx, int cy, int radius, int thickness,
                       int start_deg, int end_deg,
                       uint16_t fg, uint16_t track, uint16_t bg);
uint16_t *display_band_buffer(void);
void display_band_push(int x0, int y, int w, int lines);
void display_frame_begin(void);
void display_frame_end(void);
