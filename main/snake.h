/*
 * 贪吃蛇游戏。
 *
 * 棋盘 12x12，每格 21px，居中于圆屏（252x252，四角仍在可视圆内）。
 * 逻辑与渲染都在本模块内，主任务每 20ms 调用一次 snake_poll()。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief 开一局新游戏并绘制棋盘。 */
void snake_start(void);

/** @brief 每 20ms 调用一次；内部按节拍推进，未到时间直接返回。 */
void snake_poll(void);

/**
 * @brief 改变方向（相对当前方向，不允许 180° 掉头）。
 * @param dx -1/0/1
 * @param dy -1/0/1
 */
void snake_turn(int dx, int dy);

/** @brief 当前是否已结束。 */
bool snake_is_over(void);

/** @brief 当前得分（吃到的食物数）。 */
int snake_score(void);
