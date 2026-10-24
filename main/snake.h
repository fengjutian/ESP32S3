/*
 * 贪吃蛇游戏。
 *
 * 布局（360x360 圆屏）：
 *   棋盘 10x10，每格 21px，居中于 (75,75)..(285,285)
 *   四边外侧是方向键，点按即转向；也支持按住拖动转向
 *   顶部一行显示得分与最高分，暂停/结束提示也在这行
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>

/** @brief 开一局新游戏并绘制棋盘。 */
void snake_start(void);

/** @brief 每 20ms 调用一次；内部按节拍推进。 */
void snake_poll(void);

/** @brief 改变方向（不允许 180° 掉头）。 */
void snake_turn(int dx, int dy);

/** @brief 命中方向键则转向，返回是否命中。 */
bool snake_tap_at(int x, int y);

/** @brief 命中右上角「退出」按钮返回 true（表盘任务据此切走）。 */
bool snake_exit_hit(int x, int y);

/** @brief 暂停/继续。 */
void snake_toggle_pause(void);

bool snake_is_over(void);
bool snake_is_paused(void);
int snake_score(void);
int snake_best(void);
