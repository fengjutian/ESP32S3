#pragma once
#include "display.h"

#define UI_RING_CX 180
#define UI_RING_CY 180
#define UI_RING_R  172
#define UI_RING_W  6

void ui_disc(uint16_t *buf, int w, int y0, int lines, int rx0,
             int cx, int cy, int r, uint16_t c);
void ui_px(uint16_t *buf, int w, int y0, int lines, int rx0, int x, int y, uint16_t c);
void ui_polar(int cx, int cy, int deg, int r, int *x, int *y);
