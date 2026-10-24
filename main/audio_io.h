/*
 * 板载音频驱动接口。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief 初始化 I2S + 两个 codec。可重复调用。 */
esp_err_t audio_init(void);

/** @brief 初始化是否成功（失败时界面会提示"音频不可用"）。 */
bool audio_ready(void);

/** @brief 开始录音：打开 I2S 时钟、清掉残留帧。 */
esp_err_t audio_record_begin(void);

/**
 * @brief 读一帧双声道数据（16bit，交织）。
 *
 * @param max_frames 最多读多少「帧」（一帧 = 左右各一个样本）
 * @return 实际读到的帧数，0 表示失败
 */
int audio_record_read(int16_t *dst, int max_frames);

/** @brief 结束录音并关掉时钟。 */
esp_err_t audio_record_end(void);

/**
 * @brief 播放 16kHz 单声道 16bit PCM（内部复制成立体声送给 ES8311）。
 *
 * @param cancel 置 true 可中途停止（可为 NULL）
 */
esp_err_t audio_play(const int16_t *pcm, size_t samples, volatile bool *cancel);

/** @brief 手动开关功放（调试用）。 */
void audio_pa_enable(bool on);
