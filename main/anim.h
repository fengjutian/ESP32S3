/*
 * 弹球动画表盘。
 *
 * 圆屏里 8 个球，在圆形边界内弹跳，球与球之间是弹性碰撞。
 * 每个球带 5 帧拖尾，相撞时在接触点炸开一圈涟漪。
 *
 * 重力来自 QMI8658：平放时加速度只有 z 轴分量，等于没有重力；
 * 倾斜手表时 x/y 分量把球往低处拉。轻触屏幕会从触点把球踢开。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>

/** @brief 重置球局并整页重绘（从 face_render() 进入本表盘时调用）。 */
void anim_start(void);

/** @brief 表盘任务每 20ms 调用一次；内部按 ~30fps 推进并只重绘脏矩形。 */
void anim_poll(void);

/** @brief 变暗/熄屏时暂停渲染，点亮后恢复（省电，屏幕保留最后一帧）。 */
void anim_set_paused(bool paused);

/** @brief 在 (x, y) 施加一次冲量，把附近的球踢开。 */
void anim_kick(int x, int y);
