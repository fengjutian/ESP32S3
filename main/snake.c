/*
 * 贪吃蛇游戏实现。
 *
 * 坐标：棋盘原点 (54,54)，每格 21px，共 12x12 格。
 * 渲染策略：只在移动时擦掉尾巴、画新头，吃到食物时补画食物——不做整屏刷新。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "snake.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "display.h"
#include "ui.h"

#define GRID        12
#define CELL        21
#define ORIGIN_X    54
#define ORIGIN_Y    54
#define BOARD_PX    (GRID * CELL)

#define COLOR_BORDER  0x4208
#define COLOR_BODY    0x07E0
#define COLOR_HEAD    0x07FF
#define COLOR_FOOD    0xF800

#define SPEED_START_MS  220
#define SPEED_MIN_MS    90
#define SPEED_STEP_MS   5

typedef struct {
    int8_t x;
    int8_t y;
} cell_t;

static cell_t s_body[GRID * GRID];   /* index 0 = 蛇头 */
static int s_len;
static int8_t s_dx, s_dy;            /* 当前方向 */
static int8_t s_ndx, s_ndy;          /* 下一拍方向 */
static cell_t s_food;
static int s_score;
static bool s_over;
static int s_interval_ms;
static int64_t s_next_move_us;

static void cell_draw(int x, int y, uint16_t color)
{
    display_fill_rect(ORIGIN_X + x * CELL + 1, ORIGIN_Y + y * CELL + 1,
                      CELL - 2, CELL - 2, color);
}

static void draw_border(void)
{
    display_fill_rect(ORIGIN_X - 2, ORIGIN_Y - 2, BOARD_PX + 4, 2, COLOR_BORDER);
    display_fill_rect(ORIGIN_X - 2, ORIGIN_Y + BOARD_PX, BOARD_PX + 4, 2, COLOR_BORDER);
    display_fill_rect(ORIGIN_X - 2, ORIGIN_Y - 2, 2, BOARD_PX + 4, COLOR_BORDER);
    display_fill_rect(ORIGIN_X + BOARD_PX, ORIGIN_Y - 2, 2, BOARD_PX + 4, COLOR_BORDER);
}

static void draw_hud(void)
{
    char buf[32];

    display_fill_rect(0, 14, LCD_H_RES, 28, COLOR_BLACK);
    snprintf(buf, sizeof(buf), "贪吃蛇   得分 %d", s_score);
    ui_text_center(20, buf, 1, COLOR_WHITE);
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

static void draw_game_over(void)
{
    char buf[32];

    display_fill_rect(ORIGIN_X + 10, ORIGIN_Y + 84, BOARD_PX - 20, 84, COLOR_BLACK);

    ui_text_center(ORIGIN_Y + 92, "游戏结束", 2, COLOR_RED);

    snprintf(buf, sizeof(buf), "得分 %d", s_score);
    ui_text_center(ORIGIN_Y + 134, buf, 1, COLOR_WHITE);

    ui_text_center(ORIGIN_Y + 152, "轻触重来", 1, COLOR_GRAY);
}

static void step(void)
{
    const cell_t head = s_body[0];
    const cell_t next = {(int8_t)(head.x + s_dx), (int8_t)(head.y + s_dy)};

    if (next.x < 0 || next.x >= GRID || next.y < 0 || next.y >= GRID) {
        s_over = true;
        draw_game_over();
        return;
    }

    /* 撞到自己（尾巴会在本拍让开，所以跳过最后一节） */
    if (cell_in_body(next.x, next.y, 1)) {
        s_over = true;
        draw_game_over();
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
        draw_hud();
        place_food();
    }

    cell_draw(next.x, next.y, COLOR_HEAD);
    if (s_len > 1) {
        cell_draw(s_body[1].x, s_body[1].y, COLOR_BODY);
    }
}

void snake_start(void)
{
    display_clear(COLOR_BLACK);
    draw_border();
    draw_hud();

    s_len = 3;
    s_body[0] = (cell_t){6, 6};
    s_body[1] = (cell_t){5, 6};
    s_body[2] = (cell_t){4, 6};

    for (int i = 0; i < s_len; i++) {
        cell_draw(s_body[i].x, s_body[i].y, (i == 0) ? COLOR_HEAD : COLOR_BODY);
    }

    s_dx = 1;
    s_dy = 0;
    s_ndx = 1;
    s_ndy = 0;
    s_score = 0;
    s_over = false;
    s_interval_ms = SPEED_START_MS;
    s_next_move_us = esp_timer_get_time() + s_interval_ms * 1000LL;

    place_food();
    draw_hud();
}

void snake_poll(void)
{
    if (s_over) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now < s_next_move_us) {
        return;
    }
    s_next_move_us = now + s_interval_ms * 1000LL;

    /* 应用缓冲的方向（禁止 180° 掉头） */
    if (!(s_ndx == -s_dx && s_ndy == -s_dy)) {
        s_dx = s_ndx;
        s_dy = s_ndy;
    }

    step();
}

void snake_turn(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    /* 不允许直接反向，交给下一拍生效 */
    if (dx == -s_dx && dy == -s_dy) {
        return;
    }
    if (dx != s_ndx || dy != s_ndy) {
        ESP_LOGI("snake", "turn -> (%d,%d)  cur (%d,%d)", dx, dy, s_dx, s_dy);
    }
    s_ndx = (int8_t)dx;
    s_ndy = (int8_t)dy;
}

bool snake_is_over(void)
{
    return s_over;
}

int snake_score(void)
{
    return s_score;
}
