/*
 * 本地意图：时间、日期、天气、电量、步数这类问题直接本地回答，
 * 不走大模型（省一次请求、省 token、快 2~3 秒）。
 *
 * 匹配用的是中文关键词，够用就好；匹配不上就返回 false，
 * 交给云端大模型处理。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "local_intent.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "pedometer.h"
#include "ui.h"
#include "watch.h"
#include "weather.h"

static bool has(const char *text, const char *kw)
{
    return strstr(text, kw) != NULL;
}

bool local_intent_match(const char *text, char *answer, size_t answer_sz)
{
    if (!text || !answer || answer_sz < 16 || text[0] == '\0') {
        return false;
    }

    rtc_time_t t = {0};
    watch_get_time(&t);

    /* 时间 */
    if (has(text, "几点") || has(text, "时间") || has(text, "时刻")) {
        if (t.minute == 0) {
            snprintf(answer, answer_sz, "现在是 %d 点整。", t.hour);
        } else {
            snprintf(answer, answer_sz, "现在是 %d 点 %d 分。", t.hour, t.minute);
        }
        return true;
    }

    /* 日期 / 星期 */
    if (has(text, "几号") || has(text, "日期") || has(text, "星期") ||
        has(text, "礼拜") || has(text, "几月")) {
        snprintf(answer, answer_sz, "今天是 %d 月 %d 日，%s。",
                 t.month, t.day, ui_week_cn[t.weekday % 7]);
        return true;
    }

    /* 天气（数据来自每 30 分钟一次的 Open-Meteo 缓存） */
    if (has(text, "天气") || has(text, "气温") || has(text, "温度") ||
        has(text, "下雨") || has(text, "冷不冷") || has(text, "热不热")) {
        weather_info_t w;
        weather_get(&w);
        if (!w.valid) {
            snprintf(answer, answer_sz, "天气还没拉到，联网后我再告诉你。");
        } else {
            snprintf(answer, answer_sz, "%s%s，%d 度，湿度 %d%%，今天 %d 到 %d 度。",
                     WEATHER_CITY, weather_text(w.code), w.temp_c,
                     w.humidity, w.tmin_c, w.tmax_c);
        }
        return true;
    }

    /* 电量 */
    if (has(text, "电量") || has(text, "电池") || has(text, "多少电") ||
        has(text, "还有电") || has(text, "几格电")) {
        if (ui_soc < 0) {
            snprintf(answer, answer_sz, "电量暂时读不到。");
        } else {
            snprintf(answer, answer_sz, "电量 %d%%，电压 %.2f 伏。",
                     ui_soc, (double)ui_mv / 1000.0);
        }
        return true;
    }

    /* 步数 */
    if (has(text, "步数") || has(text, "走了多少") || has(text, "几步")) {
        snprintf(answer, answer_sz, "今天走了 %d 步，目标是 %d 步。",
                 pedometer_steps(), pedometer_goal());
        return true;
    }

    /* 身份 / 客套 */
    if (has(text, "你是谁") || has(text, "你叫什么") || has(text, "你的名字")) {
        snprintf(answer, answer_sz, "我是这块手表上的语音助手。");
        return true;
    }
    if (has(text, "谢谢") || has(text, "多谢")) {
        snprintf(answer, answer_sz, "不客气。");
        return true;
    }
    if (has(text, "再见") || has(text, "拜拜")) {
        snprintf(answer, answer_sz, "好，再见。");
        return true;
    }

    return false;
}
