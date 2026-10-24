/*
 * 本地意图匹配（时间 / 日期 / 天气 / 电量 / 步数…）。
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 尝试用本地数据回答问题。
 *
 * @param text      语音识别出来的文本
 * @param answer    输出缓冲（建议 >= 128 字节）
 * @param answer_sz 缓冲大小
 * @return true  = 已本地回答，调用者不要再请求大模型
 *         false = 没匹配上，交给云端
 */
bool local_intent_match(const char *text, char *answer, size_t answer_sz);
