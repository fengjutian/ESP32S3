/*
 * 贪吃蛇游戏实现。
 *
 * 渲染策略：只在移动时擦尾巴、画新头，吃到食物才补画食物——不做整屏刷新。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "snake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "nvs.h"

#include "display.h"
#include "ui.h"

#define TAG         "snake"

#define GRID        18
#define CELL        11
#define BOARD_PX    (GRID * CELL)
#define ORIGIN_X    ((LCD_H_RES - BOARD_PX) / 2)
#define ORIGIN_Y    ((LCD_V_RES - BOARD_PX) / 2)

#define COLOR_BORDER  0x4208
#define COLOR_BODY    0x07E0
#define COLOR_HEAD    0x07FF
#define COLOR_FOOD    0xF800

#define SPEED_START_MS  170
#define SPEED_MIN_MS    75
#define SPEED_STEP_MS   4

#define NVS_NAMESPACE   "snake"
#define NVS_KEY_BEST    "best"

typedef struct {
    int8_t x;
    int8_t y;
} cell_t;

typedef struct {
    int x, y, w, h;
    int8_t dx, dy;
    const char *glyph;
} button_t;

static const button_t s_buttons[4] = {
    {148,  16, 64, 46,  0, -1, "↑"},
    {148, 298, 64, 46,  0,  1, "↓"},
    {  6, 157, 64, 46, -1,  0, "←"},
    {290, 157, 64, 46,  1,  0, "→"},
};

static cell_t s_body[GRID * GRID];   /* index 0 = 蛇头 */
static int s_len;
static int8_t s_dx, s_dy;            /* 当前方向 */
static int8_t s_ndx, s_ndy;          /* 下一拍方向 */
static cell_t s_food;
static int s_score;
static int s_best = -1;
static bool s_over;
static bool s_paused;
static int s_interval_ms;
static int64_t s_next_move_us;

/* ---------------- 最高分（NVS） ---------------- */

static void load_best(void)
{
    if (s_best >= 0) {
        return;
    }
    s_best = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint16_t v = 0;
        if (nvs_get_u16(h, NVS_KEY_BEST, &v) == ESP_OK) {
            s_best = v;
        }
        nvs_close(h);
    }
}

static void save_best(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY_BEST, (uint16_t)s_best);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- 绘制 ---------------- */

static void cell_draw(int x, int y, uint16_t color)
{
    display_fill_rect(ORIGIN_X + x * CELL + 1, ORIGIN_Y + y * CELL + 1,
                      CELL - 2, CELL - 2, color);
}

static void draw_board(void)
{
    display_fill_rect(ORIGIN_X - 2, ORIGIN_Y - 2, BOARD_PX + 4, 2, COLOR_BORDER);
    display_fill_rect(ORIGIN_X - 2, ORIGIN_Y + BOARD_PX, BOARD_PX + 4, 2, COLOR_BORDER);
    display_fill_rect(ORIGIN_X - 2, ORIGIN_Y - 2, 2, BOARD_PX + 4, COLOR_BORDER);
    display_fill_rect(ORIGIN_X + BOARD_PX, ORIGIN_Y - 2, 2, BOARD_PX + 4, COLOR_BORDER);
}

static void draw_button(const button_t *b)
{
    /* 外框 + 内填充，让按钮在暗屏下也能看清 */
    display_fill_rect(b->x, b->y, b->w, b->h, 0x7BEF);
    display_fill_rect(b->x + 2, b->y + 2, b->w - 4, b->h - 4, 0x2945);

    const int tw = display_text_width_utf8(b->glyph, 2);
    display_draw_text_utf8(b->x + (b->w - tw) / 2, b->y + (b->h - 32) / 2,
                           b->glyph, 2, COLOR_WHITE, 0x2945);
}

static void draw_buttons(void)
{
    for (int i = 0; i < 4; i++) {
        draw_button(&s_buttons[i]);
    }
}

static void draw_hud(void)
{
    char buf[40];

    display_fill_rect(0, 0, LCD_H_RES, 20, COLOR_BLACK);

    if (s_paused) {
        snprintf(buf, sizeof(buf), "已暂停 · 轻触继续");
    } else {
        snprintf(buf, sizeof(buf), "得分 %d   最高 %d", s_score, s_best);
    }
    ui_text_center(2, buf, 1, COLOR_WHITE);
}

static void draw_game_over(void)
{
    char buf[40];

    display_fill_rect(ORIGIN_X + 4, ORIGIN_Y + 76, BOARD_PX - 8, 88, COLOR_BLACK);
    ui_text_center(ORIGIN_Y + 82, "游戏结束", 2, COLOR_RED);

    snprintf(buf, sizeof(buf), "得分 %d   最高 %d", s_score, s_best);
    ui_text_center(ORIGIN_Y + 124, buf, 1, COLOR_WHITE);

    ui_text_center(ORIGIN_Y + 144, "轻触重来", 1, COLOR_GRAY);
}

static bool cell_in_body(int x, int y, int skip_tail)
{
    const int n = s_len - (skip_tail ? 1 : 0);
    for (int i = 0; i < n; i++) {
        if (s_body[i].x == x && s_body[i].y == y) {
            return true;
        }
    }
    return false;
}

static void place_food(void)
{
    for (int attempt = 0; attempt < GRID * GRID * 4; attempt++) {
        const int x = (int)(esp_random() % GRID);
        const int y = (int)(esp_random() % GRID);
        if (!cell_in_body(x, y, 0)) {
            s_food.x = (int8_t)x;
            s_food.y = (int8_t)y;
            cell_draw(x, y, COLOR_FOOD);
            return;
        }
    }
}

/* ---------------- 逻辑 ---------------- */

static void step(void)
{
    const cell_t head = s_body[0];
    const cell_t next = {(int8_t)(head.x + s_dx), (int8_t)(head.y + s_dy)};

    if (next.x < 0 || next.x >= GRID || next.y < 0 || next.y >= GRID) {
        s_over = true;
        return;
    }

    /* 撞到自己（尾巴本拍会让开，跳过最后一节） */
    if (cell_in_body(next.x, next.y, 1)) {
        s_over = true;
        return;
    }

    const bool ate = (next.x == s_food.x && next.y == s_food.y);

    if (!ate) {
        cell_draw(s_body[s_len - 1].x, s_body[s_len - 1].y, COLOR_BLACK);
    }

    memmove(&s_body[1], &s_body[0], sizeof(cell_t) * (size_t)(s_len - 1));
    s_body[0] = next;

    if (ate) {
        s_len++;
        s_score++;
        if (s_interval_ms > SPEED_MIN_MS) {
            s_interval_ms -= SPEED_STEP_MS;
        }
        place_food();
        draw_hud();
    }

    cell_draw(next.x, next.y, COLOR_HEAD);
    if (s_len > 1) {
        cell_draw(s_body[1].x, s_body[1].y, COLOR_BODY);
    }
}

void snake_start(void)
{
    load_best();

    display_clear(COLOR_BLACK);
    draw_board();
    draw_buttons();

    s_len = 3;
    s_body[0] = (cell_t){8, 9};
    s_body[1] = (cell_t){7, 9};
    s_body[2] = (cell_t){6, 9};

    for (int i = 0; i < s_len; i++) {
        cell_draw(s_body[i].x, s_body[i].y, (i == 0) ? COLOR_HEAD : COLOR_BODY);
    }

    s_dx = 1;
    s_dy = 0;
    s_ndx = 1;
    s_ndy = 0;
    s_score = 0;
    s_over = false;
    s_paused = false;
    s_interval_ms = SPEED_START_MS;
    s_next_move_us = esp_timer_get_time() + s_interval_ms * 1000LL;

    place_food();
    draw_hud();
}

void snake_poll(void)
{
    if (s_over || s_paused) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now < s_next_move_us) {
        return;
    }
    s_next_move_us = now + s_interval_ms * 1000LL;

    if (!(s_ndx == -s_dx && s_ndy == -s_dy)) {
        s_dx = s_ndx;
        s_dy = s_ndy;
    }

    step();

    if (s_over) {
        if (s_score > s_best) {
            s_best = s_score;
            save_best();
        }
        ESP_LOGI(TAG, "game over: score=%d best=%d", s_score, s_best);
        draw_game_over();
        draw_hud();
    }
}

void snake_turn(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    if (dx == -s_dx && dy == -s_dy) {
        return;   /* 不允许 180° 掉头 */
    }
    if (dx != s_ndx || dy != s_ndy) {
        ESP_LOGI(TAG, "turn -> (%d,%d)  cur (%d,%d)", dx, dy, s_dx, s_dy);
    }
    s_ndx = (int8_t)dx;
    s_ndy = (int8_t)dy;
}

bool snake_tap_at(int x, int y)
{
    for (int i = 0; i < 4; i++) {
        const button_t *b = &s_buttons[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            ESP_LOGI(TAG, "tap (%d,%d) -> button %s", x, y, b->glyph);
            snake_turn(b->dx, b->dy);
            return true;
        }
    }

    /* 兜底：点在棋盘外侧任何位置，按相对圆心的角度判方向 */
    const int dx = x - 180;
    const int dy = y - 180;
    if (abs(dx) > BOARD_PX / 2 + 6 || abs(dy) > BOARD_PX / 2 + 6) {
        if (abs(dx) >= abs(dy)) {
            ESP_LOGI(TAG, "tap (%d,%d) -> edge %s", x, y, dx > 0 ? "right" : "left");
            snake_turn(dx > 0 ? 1 : -1, 0);
        } else {
            ESP_LOGI(TAG, "tap (%d,%d) -> edge %s", x, y, dy > 0 ? "down" : "up");
            snake_turn(0, dy > 0 ? 1 : -1);
        }
        return true;
    }

    return false;
}

void snake_toggle_pause(void)
{
    if (s_over) {
        return;
    }
    s_paused = !s_paused;
    if (!s_paused) {
        s_next_move_us = esp_timer_get_time() + s_interval_ms * 1000LL;
    }
    draw_hud();
    ESP_LOGI(TAG, "%s", s_paused ? "paused" : "resumed");
}

bool snake_is_over(void)
{
    return s_over;
}

bool snake_is_paused(void)
{
    return s_paused;
}

int snake_score(void)
{
    return s_score;
}

int snake_best(void)
{
    load_best();
    return s_best;
}
