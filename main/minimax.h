/*
 * MiniMax 云端接口：语音识别 / 大模型对话 / 语音合成。
 *
 * 三个接口都是 HTTPS + JSON（ASR 是 multipart 上传），
 * 数据量大的缓冲区全部放 PSRAM，内部 512KB SRAM 不动。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief 合成出来的一段 PCM（16kHz / 单声道 / 16bit 小端）。用完调 mm_free()。 */
typedef struct {
    uint8_t *pcm;
    size_t   len;
} mm_audio_t;

/** @brief 是否填了 API Key。 */
bool mm_ready(void);

/**
 * @brief 语音识别：WAV（16k 单声道 16bit）→ 文本。
 *
 * @param wav     完整 WAV 文件（含 44 字节头）
 * @param wav_len 字节数
 * @param text    输出缓冲
 */
esp_err_t mm_asr(const uint8_t *wav, size_t wav_len, char *text, size_t text_sz);

/**
 * @brief 调大模型（OpenAI 兼容的 /v1/chat/completions），返回纯文本回答。
 */
esp_err_t mm_chat(const char *user_text, char *reply, size_t reply_sz);

/**
 * @brief 语音合成：文本 → PCM。成功时 out->pcm 需要调用 mm_free()。
 */
esp_err_t mm_tts(const char *text, mm_audio_t *out);

/** @brief 释放 mm_* 返回的缓冲区。 */
void mm_free(void *ptr);
