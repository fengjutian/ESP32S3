/*
 * 语音问答：按住说话 → MiniMax ASR → 大模型 → TTS → 喇叭。
 *
 * 所有网络和音频操作都在 ai 任务里串行执行，表盘任务只负责画界面，
 * 避免两边同时碰 I2S / 显示。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>

#include "rtc.h"

/** @brief 初始化音频和后台任务（app_main 里调用一次）。 */
void voice_ai_init(void);

/** @brief 按下「说话」：开始一轮问答。 */
void voice_ai_ptt_press(void);

/**
 * @brief 松手：结束录音并上传。
 *
 * @param discard true = 按得太短（当误触），直接取消这一轮
 */
void voice_ai_ptt_release(bool discard);

/** @brief 取消当前这一轮（滑动切走表盘时用）。 */
void voice_ai_cancel(void);

/** @brief 重播上一轮的回答（没有就问用户先说一句）。 */
void voice_ai_replay(void);

/** @brief 是否正在处理一轮问答。 */
bool voice_ai_busy(void);

/** @brief 画 AI 表盘（由表盘任务调用，内容没变时自己跳过）。 */
void voice_ai_render(const rtc_time_t *t);

/** @brief 强制下次 render 重画（切表盘回来时用）。 */
void voice_ai_invalidate(void);
