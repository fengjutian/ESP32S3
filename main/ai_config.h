/*
 * MiniMax 接入配置。
 *
 * 密钥不要写在这里（这个文件会进 git）。请写到 main/app_config.h：
 *
 *     #define MINIMAX_API_KEY  "sk-api-xxxxxxxx"
 *
 * 下面每一个宏都可以在 app_config.h 里覆盖。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include "app_config.h"

/* ---------------- 账号 / 网关 ---------------- */

/* 国内账号用 https://api.minimax.cn，海外账号用 https://api.minimax.io。
 * key 和区域是绑定的，填错会 401 / 404。 */
#ifndef MINIMAX_BASE_URL
#define MINIMAX_BASE_URL        "https://api.minimax.io"
#endif

#ifndef MINIMAX_API_KEY
#define MINIMAX_API_KEY         ""
#endif

/* ---------------- 模型 ---------------- */

/* 当前在售文本模型：MiniMax-M3.1-Flash / MiniMax-M3（都是 1M 上下文） */
#ifndef MINIMAX_CHAT_MODEL
#define MINIMAX_CHAT_MODEL      "MiniMax-M3.1-Flash"
#endif

/* 语音识别：目前公开模型只有 asr-1.0 */
#ifndef MINIMAX_ASR_MODEL
#define MINIMAX_ASR_MODEL       "asr-1.0"
#endif

/* 语音合成：speech-2.8-turbo（快）/ speech-2.8-hd（更自然） */
#ifndef MINIMAX_TTS_MODEL
#define MINIMAX_TTS_MODEL       "speech-2.8-turbo"
#endif

/* 音色 ID，可在「系统音色列表」里换。留空则用默认中文女声 */
#ifndef MINIMAX_TTS_VOICE
#define MINIMAX_TTS_VOICE       "Chinese (Mandarin)_Lyrical_Voice"
#endif

#ifndef MINIMAX_TTS_SPEED
#define MINIMAX_TTS_SPEED       1.0f
#endif

/* ---------------- 语音问答参数 ---------------- */

/* 单次录音上限（毫秒） */
#ifndef AI_MAX_RECORD_MS
#define AI_MAX_RECORD_MS        8000
#endif

/* 录音采样率。ASR 要求单声道 16kHz，别改 */
#define AI_SAMPLE_RATE          16000
#define AI_CHANNELS             1

/* 系统提示词：手表屏幕小、喇叭小，回答要短 */
#ifndef AI_SYSTEM_PROMPT
#define AI_SYSTEM_PROMPT        "你是一块智能手表的语音助手。回答用口语化的中文，"\
                                "控制在80字以内，不要用 Markdown、列表或表情符号。"
#endif

/* 回答 token 上限，越小越快 */
#ifndef AI_CHAT_MAX_TOKENS
#define AI_CHAT_MAX_TOKENS      320
#endif

/* M3.1-Flash 总是思考，low 最快；可选 low/medium/high/xhigh/max */
#ifndef AI_REASONING_EFFORT
#define AI_REASONING_EFFORT     "low"
#endif

/* 合成语音的最大字数（超出的截断，省流量也省时间） */
#ifndef AI_TTS_MAX_CHARS
#define AI_TTS_MAX_CHARS        300
#endif

/* 单次 HTTPS 请求超时 */
#ifndef AI_HTTP_TIMEOUT_MS
#define AI_HTTP_TIMEOUT_MS      30000
#endif

/* 开机自检：启动时用「你好，用一句话证明你在工作」跑一次文本对话，
 * 串口会打 `selftest OK: ...`。平时保持 0。 */
#ifndef AI_SELFTEST_ON_BOOT
#define AI_SELFTEST_ON_BOOT     0
#endif

/* ---------------- 音频硬件 ---------------- */

/* ES7210 选哪几路麦克风：bit0=MIC1 … bit3=MIC4。
 * 默认前两路；如果录出来是静音，改成 0x0C（MIC3|MIC4）再试。 */
#ifndef AI_MIC_SELECT
#define AI_MIC_SELECT           0x03
#endif

/* 麦克风增益（dB，ES7210 最大 37.5） */
#ifndef AI_MIC_GAIN_DB
#define AI_MIC_GAIN_DB          24.0f
#endif

/* 喇叭音量 0~100 */
#ifndef AI_SPEAKER_VOLUME
#define AI_SPEAKER_VOLUME       80
#endif
