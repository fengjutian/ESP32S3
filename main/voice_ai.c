/*
 * 语音问答状态机。
 *
 * 一轮问答（全部在 ai 任务里串行）：
 *   按住 -> 录音（16k 单声道，最长 8 秒）
 *   松手 -> WAV 打包 -> MiniMax ASR
 *        -> 大模型 /v1/chat/completions
 *        -> TTS /v1/t2a_v2（hex PCM）
 *        -> ES8311 播放
 *
 * 表盘任务只调用 voice_ai_render() 画界面，两边用 s_mux 同步文字。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "voice_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ai_config.h"
#include "audio_io.h"
#include "display.h"
#include "local_intent.h"
#include "minimax.h"
#include "net_link.h"
#include "ui.h"

static const char *TAG = "voice";

#define AI_TASK_STACK   20480
#define AI_TASK_PRIO    5
#define CMD_TALK        1
#define CMD_REPLAY      2

typedef enum {
    ST_IDLE = 0,
    ST_CONNECT,
    ST_LISTEN,
    ST_ASR,
    ST_THINK,
    ST_SPEAK,
    ST_ERROR,
} state_t;

static SemaphoreHandle_t s_cmd;
static SemaphoreHandle_t s_mux;

static volatile bool s_busy;
static volatile bool s_stop;     /* 松手 */
static volatile bool s_abort;    /* 取消 / 误触 */
static volatile bool s_barge_in; /* 处理中再按 = 打断 */
static volatile bool s_want_replay;
static volatile int s_pending_cmd;
static volatile bool s_dirty = true;

static state_t s_state = ST_IDLE;
static int64_t s_state_since_us;   /* 当前状态开始时间，用来在屏幕上显示已等待秒数 */
static char s_status[32] = "按住说话";
static char s_question[192];
static char s_answer[768];

/* 录音缓冲：44 字节 WAV 头 + PCM（PSRAM） */
static uint8_t *s_wav;
static size_t s_wav_pcm_cap;

/* 本轮合成出来的 PCM（边收边播，同时留一份给"重播"），PSRAM */
static uint8_t *s_pcm_buf;
static size_t s_pcm_cap;
static volatile size_t s_pcm_len;

/* 测「从发起合成到第一块音频」的延迟 */
static int64_t s_tts_req_us;
static bool s_tts_first_done;

/* 流式 TTS 回调：收到一块就写一块 I2S，返回 false 表示打断 */
static bool tts_pcm_cb(const uint8_t *pcm, size_t bytes, void *ctx)
{
    (void)ctx;

    if (s_abort) {
        return false;
    }
    if (!s_tts_first_done) {
        s_tts_first_done = true;
        ESP_LOGI(TAG, "first pcm after %d ms", (int)((esp_timer_get_time() - s_tts_req_us) / 1000));
    }
    if (s_pcm_buf && s_pcm_len + bytes <= s_pcm_cap) {
        memcpy(s_pcm_buf + s_pcm_len, pcm, bytes);
        s_pcm_len += bytes;
    }
    if (audio_play_write((const int16_t *)pcm, bytes / 2) != ESP_OK) {
        return false;
    }
    return !s_abort;
}

/* ---------------- 小工具 ---------------- */

static void ui_set(state_t st, const char *status)
{
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (st != s_state) {
            s_state = st;
            s_state_since_us = esp_timer_get_time();
        }
        if (status) {
            snprintf(s_status, sizeof(s_status), "%s", status);
        }
        s_dirty = true;
        xSemaphoreGive(s_mux);
    }
}

static void ui_set_question(const char *text)
{
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        snprintf(s_question, sizeof(s_question), "%s", text ? text : "");
        s_dirty = true;
        xSemaphoreGive(s_mux);
    }
}

static void ui_set_answer(const char *text)
{
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        snprintf(s_answer, sizeof(s_answer), "%s", text ? text : "");
        s_dirty = true;
        xSemaphoreGive(s_mux);
    }
}

static void wav_header(uint8_t *p, uint32_t pcm_bytes)
{
    uint32_t u32;
    uint16_t u16;

    memcpy(p, "RIFF", 4);
    u32 = 36 + pcm_bytes;
    memcpy(p + 4, &u32, 4);
    memcpy(p + 8, "WAVEfmt ", 8);
    u32 = 16;
    memcpy(p + 16, &u32, 4);
    u16 = 1;                       /* PCM */
    memcpy(p + 20, &u16, 2);
    u16 = 1;                       /* 单声道 */
    memcpy(p + 22, &u16, 2);
    u32 = AI_SAMPLE_RATE;
    memcpy(p + 24, &u32, 4);
    u32 = AI_SAMPLE_RATE * 2;      /* 字节率 */
    memcpy(p + 28, &u32, 4);
    u16 = 2;                       /* block align */
    memcpy(p + 32, &u16, 2);
    u16 = 16;
    memcpy(p + 34, &u16, 2);
    memcpy(p + 36, "data", 4);
    u32 = pcm_bytes;
    memcpy(p + 40, &u32, 4);
}

/* ---------------- 一轮问答 ---------------- */

static void do_replay(void);

static void do_turn(void)
{
    bool acquired = false;
    int16_t frame[480 * 2];        /* 30ms 一帧，双声道 */

    if (s_abort) {
        goto out;   /* 误触/取消：如果手里有回答，out 里会重播 */
    }

    if (!audio_ready()) {
        ui_set(ST_ERROR, "音频初始化失败");
        return;
    }
    if (!mm_ready()) {
        ui_set(ST_ERROR, "未配置 API Key");
        return;
    }

    ui_set_question("");
    ui_set_answer("");

    ui_set(ST_CONNECT, "连网中…");
    if (!net_link_acquire(20000)) {
        ui_set(ST_ERROR, "WiFi 未连接");
        return;
    }
    acquired = true;

    if (s_abort) {
        goto out;
    }

    /* ---- 1. 录音 ---- */
    ui_set(ST_LISTEN, "请说话…");

    const size_t max_frames = (size_t)AI_MAX_RECORD_MS * AI_SAMPLE_RATE / 1000;
    size_t frames = 0;

    audio_record_begin();
    while (!s_stop && !s_abort && frames < max_frames) {
        const int n = audio_record_read(frame, 480);
        if (n <= 0) {
            break;
        }
        size_t take = (size_t)n;
        if (frames + take > max_frames) {
            take = max_frames - frames;
        }
        if ((frames + take) * sizeof(int16_t) > s_wav_pcm_cap) {
            take = s_wav_pcm_cap / sizeof(int16_t) - frames;
        }
        if (take == 0) {
            break;
        }
        int16_t *dst = (int16_t *)(s_wav + 44) + frames;
        for (size_t i = 0; i < take; i++) {
            dst[i] = frame[2 * i];   /* 取左声道 */
        }
        frames += take;
    }
    audio_record_end();

    if (s_abort) {
        goto out;
    }
    if (frames < AI_SAMPLE_RATE / 4) {   /* 不到 0.25 秒 */
        ui_set(ST_IDLE, "按住说话");
        goto out;
    }

    /* 电平诊断：录不到声音时先看这里，比盲猜麦克风通路快 */
    int peak = 0;
    {
        const int16_t *pcm = (const int16_t *)(s_wav + 44);
        for (size_t i = 0; i < frames; i++) {
            int v = pcm[i];
            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }
    }
    ESP_LOGI(TAG, "recorded %u ms, peak %d", (unsigned)(frames * 1000 / AI_SAMPLE_RATE), peak);
    if (peak < 200) {
        ESP_LOGW(TAG, "mic level very low - try AI_MIC_SELECT 0x0C / raise AI_MIC_GAIN_DB");
    }

    /* ---- 2. 识别 ---- */
    wav_header(s_wav, (uint32_t)(frames * 2));
    ui_set(ST_ASR, "识别中…");

    char question[sizeof(s_question)];
    if (mm_asr(s_wav, 44 + frames * 2, question, sizeof(question)) != ESP_OK) {
        ui_set(ST_ERROR, "没听清，再说一次");
        goto out;
    }
    ui_set_question(question);

    if (s_abort) {
        goto out;
    }

    /* ---- 3. 能本地答的就别上云 ---- */
    char answer[sizeof(s_answer)];

    if (local_intent_match(question, answer, sizeof(answer))) {
        ESP_LOGI(TAG, "local: %s", answer);
        ui_set_answer(answer);
    } else {
        ui_set(ST_THINK, "思考中…");
        if (mm_chat(question, answer, sizeof(answer)) != ESP_OK) {
            ui_set(ST_ERROR, "大模型请求失败");
            goto out;
        }
        ui_set_answer(answer);
    }

    if (s_abort) {
        goto out;
    }

    /* ---- 4. 流式合成并播放：收到第一段就出声 ---- */
    ui_set(ST_SPEAK, "回答中…");

    if (!s_abort) {
        s_pcm_len = 0;
        s_tts_first_done = false;
        s_tts_req_us = esp_timer_get_time();
        audio_play_begin();
        const esp_err_t tts_err = mm_tts_stream(answer, tts_pcm_cb, NULL);
        audio_play_end();
        if (tts_err != ESP_OK && !s_abort) {
            ESP_LOGW(TAG, "tts stream failed (%s), keep text only", esp_err_to_name(tts_err));
        }
    }

    ui_set(ST_IDLE, "按住说话");

out:
    if (acquired) {
        net_link_release();
    }

    /* 轻点（误触）之后如果手里有上一轮的回答，就当作「重播」 */
    if (s_want_replay) {
        s_want_replay = false;
        do_replay();
    }
}

static void do_replay(void)
{
    if (!s_pcm_buf || s_pcm_len < 2) {
        ui_set(ST_ERROR, "还没有回答，先按住说话");
        return;
    }
    s_abort = false;
    ui_set(ST_SPEAK, "重播…");
    audio_play((const int16_t *)s_pcm_buf, s_pcm_len / 2, &s_abort);
    ui_set(ST_IDLE, "按住说话");
}

#if AI_SELFTEST_ON_BOOT
/* 开机自检：验证 key / 网关 / TLS / JSON 解析整条链路，只打日志 */
static void ai_selftest(void)
{
    /* 先验证本地意图匹配 */
    static const char *cases[] = {
        "现在几点了", "今天几号", "今天天气怎么样", "还有多少电",
        "我走了多少步", "你是谁", "谢谢", "讲个笑话",
    };
    char lbuf[192];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (local_intent_match(cases[i], lbuf, sizeof(lbuf))) {
            ESP_LOGI(TAG, "local[%s] -> %s", cases[i], lbuf);
        } else {
            ESP_LOGI(TAG, "local[%s] -> (cloud)", cases[i]);
        }
    }

    if (!mm_ready()) {
        ESP_LOGE(TAG, "selftest: no api key");
        return;
    }
    if (!net_link_acquire(20000)) {
        ESP_LOGE(TAG, "selftest: no network");
        return;
    }

    char reply[512] = {0};
    const esp_err_t err = mm_chat("你好，用一句话证明你在工作", reply, sizeof(reply));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "selftest OK: %s", reply);

        /* 顺带验证流式 TTS：应当马上从喇叭出声 */
        s_pcm_len = 0;
        s_tts_first_done = false;
        s_tts_req_us = esp_timer_get_time();
        audio_play_begin();
        const esp_err_t terr = mm_tts_stream(reply, tts_pcm_cb, NULL);
        audio_play_end();
        ESP_LOGI(TAG, "selftest tts=%s, %u bytes", esp_err_to_name(terr), (unsigned)s_pcm_len);
    } else {
        ESP_LOGE(TAG, "selftest FAILED (err=%s)", esp_err_to_name(err));
    }
    net_link_release();
}
#endif

static void ai_task(void *arg)
{
    (void)arg;

#if AI_SELFTEST_ON_BOOT
    ai_selftest();
#endif

    for (;;) {
        if (xSemaphoreTake(s_cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const int cmd = s_pending_cmd;
        s_pending_cmd = 0;

        if (cmd == CMD_TALK) {
            do_turn();
        } else if (cmd == CMD_REPLAY) {
            do_replay();
        }

        s_busy = false;
    }
}

/* ---------------- 对外接口 ---------------- */

void voice_ai_init(void)
{
    if (s_cmd) {
        return;
    }

    s_cmd = xSemaphoreCreateBinary();
    s_mux = xSemaphoreCreateMutex();
    if (!s_cmd || !s_mux) {
        ESP_LOGE(TAG, "semaphore alloc failed");
        return;
    }

    const size_t pcm_cap = (size_t)AI_MAX_RECORD_MS * AI_SAMPLE_RATE / 1000 * sizeof(int16_t);
    s_wav_pcm_cap = pcm_cap;
    s_wav = heap_caps_malloc(pcm_cap + 44, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wav) {
        s_wav = malloc(pcm_cap + 44);
    }
    if (!s_wav) {
        ESP_LOGE(TAG, "no memory for record buffer (%u bytes)", (unsigned)pcm_cap);
        return;
    }

    /* 重播缓冲：最多留 16 秒合成音频（40 字回答约 5 秒，够用） */
    s_pcm_cap = 512 * 1024;
    s_pcm_buf = heap_caps_malloc(s_pcm_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_buf) {
        s_pcm_cap = 0;
        ESP_LOGW(TAG, "no replay buffer, replay disabled");
    }

    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed, voice disabled");
    }

    xTaskCreate(ai_task, "voice_ai", AI_TASK_STACK, NULL, AI_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "ready (%u s max, %u KB buffer)", AI_MAX_RECORD_MS / 1000,
             (unsigned)((pcm_cap + 44) / 1024));
}

void voice_ai_ptt_press(void)
{
    if (!s_cmd) {
        return;
    }
    if (s_busy) {
        /* 处理中再按一下 = 打断：停止播放、取消当前请求 */
        s_abort = true;
        s_stop = true;
        s_barge_in = true;
        return;
    }
    s_barge_in = false;
    s_stop = false;
    s_abort = false;
    s_busy = true;
    s_pending_cmd = CMD_TALK;
    s_dirty = true;
    xSemaphoreGive(s_cmd);
}

void voice_ai_ptt_release(bool discard)
{
    if (s_barge_in) {
        s_barge_in = false;   /* 这次按下只是用来打断的，不当成说话 */
        return;
    }
    if (!s_busy) {
        return;
    }
    if (discard) {
        s_abort = true;
        if (s_pcm_len >= 2) {
            s_want_replay = true;
        }
    }
    s_stop = true;
}

void voice_ai_cancel(void)
{
    if (!s_busy) {
        return;
    }
    s_abort = true;
    s_stop = true;
}

void voice_ai_replay(void)
{
    if (!s_cmd || s_busy) {
        return;
    }
    s_busy = true;
    s_abort = false;
    s_pending_cmd = CMD_REPLAY;
    xSemaphoreGive(s_cmd);
}

bool voice_ai_busy(void)
{
    return s_busy;
}

/* ---------------- 界面 ---------------- */

static int utf8_len(char c)
{
    const unsigned char u = (unsigned char)c;
    if (u < 0x80) return 1;
    if ((u >> 5) == 0x6) return 2;
    if ((u >> 4) == 0xE) return 3;
    if ((u >> 3) == 0x1E) return 4;
    return 1;
}

/* 按屏幕宽度折行并居中绘制，返回画了几行 */
static int draw_wrapped(const char *text, int y0, int max_lines, int line_h,
                        uint8_t scale, uint16_t color)
{
    const int max_w = LCD_H_RES - 24;
    char line[128];
    size_t o = 0;
    int lines = 0;
    int line_w = 0;

    if (!text || text[0] == '\0') {
        return 0;
    }

    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            p++;
            line[o] = '\0';
            ui_text_center(y0 + lines * line_h, line, scale, color);
            lines++;
            o = 0;
            line_w = 0;
            if (lines >= max_lines) {
                return lines;
            }
            continue;
        }

        const int clen = utf8_len(*p);
        const int w = (clen == 1) ? 8 * scale : 16 * scale;

        if (line_w + w > max_w && o > 0) {
            line[o] = '\0';
            ui_text_center(y0 + lines * line_h, line, scale, color);
            lines++;
            o = 0;
            line_w = 0;
            if (lines >= max_lines) {
                return lines;
            }
        }
        if (o + (size_t)clen + 1 < sizeof(line)) {
            memcpy(line + o, p, (size_t)clen);
            o += (size_t)clen;
        }
        line_w += w;
        p += clen;
    }

    if (o > 0) {
        line[o] = '\0';
        ui_text_center(y0 + lines * line_h, line, scale, color);
        lines++;
    }
    return lines;
}

static uint16_t state_color(state_t st)
{
    switch (st) {
    case ST_LISTEN:  return COLOR_GREEN;
    case ST_ASR:
    case ST_THINK:   return COLOR_YELLOW;
    case ST_SPEAK:   return COLOR_CYAN;
    case ST_ERROR:   return COLOR_RED;
    case ST_CONNECT: return COLOR_CYAN;
    default:         return COLOR_WHITE;
    }
}

/* 上次画到屏幕上的内容；只有变化的那一块才重画，避免全屏闪烁 */
static char s_drawn_status[48];
static char s_drawn_question[192];
static char s_drawn_answer[768];
static bool s_need_layout = true;

void voice_ai_invalidate(void)
{
    s_dirty = true;
    s_need_layout = true;
}

void voice_ai_render(const rtc_time_t *t)
{
    (void)t;

    if (!s_mux) {
        return;
    }
    /* 空闲且没变化就不用重画；处理中即使没有新内容也要走一遍，
     * 因为要把「已等待秒数」刷到屏幕上（否则看起来像卡住）。 */
    if (!s_dirty && s_state == ST_IDLE) {
        return;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    const state_t st = s_state;
    char status[48];
    char question[sizeof(s_question)];
    char answer[sizeof(s_answer)];
    snprintf(status, sizeof(status), "%s", s_status);
    snprintf(question, sizeof(question), "%s", s_question);
    snprintf(answer, sizeof(answer), "%s", s_answer);
    const int64_t since_us = s_state_since_us;
    s_dirty = false;
    xSemaphoreGive(s_mux);

    /* 处理中显示已经等了几秒，让屏幕一直在动 */
    if (st != ST_IDLE && st != ST_ERROR) {
        const int sec = (int)((esp_timer_get_time() - since_us) / 1000000);
        if (sec > 0) {
            const size_t n = strlen(status);
            if (n + 8 < sizeof(status)) {
                snprintf(status + n, sizeof(status) - n, " %ds", sec);
            }
        }
    }

    /* 首次进入：整屏铺一次静态框架 */
    if (s_need_layout) {
        s_need_layout = false;
        s_drawn_status[0] = '\0';
        s_drawn_question[0] = '\0';
        s_drawn_answer[0] = '\0';

        display_clear(COLOR_BLACK);
        ui_text_center(22, "AI 助手", 1, COLOR_GRAY);
        display_fill_rect(60, 96, LCD_H_RES - 120, 2, 0x2104);
        ui_text_center(330, "按住说话 · 轻点重播", 1, 0x4208);
    }

    /* 状态行 */
    if (strcmp(status, s_drawn_status) != 0) {
        display_fill_rect(0, 42, LCD_H_RES, 48, COLOR_BLACK);
        ui_text_center(50, status, 2, state_color(st));
        snprintf(s_drawn_status, sizeof(s_drawn_status), "%s", status);
    }

    /* 我问的 */
    if (strcmp(question, s_drawn_question) != 0) {
        display_fill_rect(0, 100, LCD_H_RES, 78, COLOR_BLACK);
        draw_wrapped(question, 112, 3, 18, 1, COLOR_GRAY);
        snprintf(s_drawn_question, sizeof(s_drawn_question), "%s", question);
    }

    /* 它答的 */
    if (strcmp(answer, s_drawn_answer) != 0) {
        display_fill_rect(0, 180, LCD_H_RES, 140, COLOR_BLACK);
        draw_wrapped(answer, 186, 7, 18, 1, COLOR_WHITE);
        snprintf(s_drawn_answer, sizeof(s_drawn_answer), "%s", answer);
    }
}
