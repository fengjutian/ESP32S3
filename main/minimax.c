/*
 * MiniMax 云端接口实现。
 *
 * 三个接口：
 *   1. POST /v1/speech_to_text   multipart/form-data，上传 16k 单声道 WAV
 *   2. POST /v1/chat/completions OpenAI 兼容，返回 choices[0].message.content
 *   3. POST /v1/t2a_v2           输出 hex 编码的 PCM（16k 单声道 16bit）
 *
 * 解析用的是极简 JSON 扫描（不引第三方库）：先找 "key" 再按 JSON 字符串
 * 规则反转义。TTS 的音频直接在响应缓冲区里原地 hex 解码，省一次 3MB 拷贝。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "minimax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "ai_config.h"

static const char *TAG = "minimax";

/* 响应体上限：够用即可，超出直接报错，避免把 PSRAM 吃光 */
#define RESP_MAX_ASR    (64 * 1024)
#define RESP_MAX_CHAT   (256 * 1024)
#define RESP_MAX_TTS    (3 * 1024 * 1024)

/* ---------------- 内存：优先 PSRAM ---------------- */

static void *mm_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void mm_free(void *ptr)
{
    free(ptr);
}

bool mm_ready(void)
{
    return strlen(MINIMAX_API_KEY) > 0;
}

/* ---------------- HTTPS 基础 ---------------- */

static esp_http_client_handle_t http_begin(const char *path, const char *ctype,
                                           size_t content_len, const char *lang)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", MINIMAX_BASE_URL, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = AI_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "http init failed");
        return NULL;
    }

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", MINIMAX_API_KEY);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", ctype);
    if (lang) {
        esp_http_client_set_header(c, "language", lang);
    }

    if (esp_http_client_open(c, (int)content_len) != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed", url);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

static bool http_write_all(esp_http_client_handle_t c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        const int chunk = (len > 4096) ? 4096 : (int)len;
        const int w = esp_http_client_write(c, (const char *)p, chunk);
        if (w <= 0) {
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

/* 读完整个响应；返回 PSRAM 里以 '\0' 结尾的字符串 */
static esp_err_t http_read_all(esp_http_client_handle_t c, char **out, size_t *out_len, size_t max)
{
    const int status = esp_http_client_get_status_code(c);
    const int64_t clen = esp_http_client_get_content_length(c);

    size_t cap = max;
    if (clen > 0 && (size_t)clen + 1 < max) {
        cap = (size_t)clen + 1;
    }

    char *buf = mm_alloc(cap + 1);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for response (%u bytes)", (unsigned)cap);
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    for (;;) {
        if (len >= cap) {
            ESP_LOGE(TAG, "response too large (>%u)", (unsigned)cap);
            mm_free(buf);
            return ESP_ERR_NO_MEM;
        }
        const int r = esp_http_client_read(c, buf + len, (int)(cap - len));
        if (r < 0) {
            ESP_LOGE(TAG, "read failed");
            mm_free(buf);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    buf[len] = '\0';

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.240s", status, buf);
        mm_free(buf);
        return ESP_FAIL;
    }

    *out = buf;
    *out_len = len;
    return ESP_OK;
}

/* ---------------- 极简 JSON ---------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/* 找到 "key": 之后的值起始位置 */
static const char *find_value(const char *json, const char *key)
{
    if (!json) {
        return NULL;
    }

    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const size_t plen = strlen(pat);

    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = skip_ws(p + plen);
        if (*q == ':') {
            return skip_ws(q + 1);
        }
        p += plen;
    }
    return NULL;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned parse_hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        const int h = hex_val(p[i]);
        v = (v << 4) | (unsigned)(h < 0 ? 0 : h);
    }
    return v;
}

static size_t utf8_put(char *out, size_t o, size_t out_sz, unsigned cp)
{
    if (cp < 0x80) {
        if (o + 1 >= out_sz) return o;
        out[o++] = (char)cp;
    } else if (cp < 0x800) {
        if (o + 2 >= out_sz) return o;
        out[o++] = (char)(0xC0 | (cp >> 6));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (o + 3 >= out_sz) return o;
        out[o++] = (char)(0xE0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (o + 4 >= out_sz) return o;
        out[o++] = (char)(0xF0 | (cp >> 18));
        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* 把 p 指向的 JSON 字符串反转义到 out（截断时保证不切坏 UTF-8）
 *
 * 注意：JSON 里的中文本来就是 UTF-8 原始字节，必须原样拷贝；
 * 只有 \uXXXX 转义才需要编码成 UTF-8。曾经的 bug 是把原始字节也当成
 * 码点重新编码，导致每个汉字被二次编码成乱码、屏幕上全变问号。 */
static bool json_decode_string(const char *p, char *out, size_t out_sz)
{
    if (!p || *p != '"' || out_sz < 2) {
        return false;
    }
    p++;

    size_t o = 0;
    while (*p && *p != '"') {
        if (*p != '\\') {
            if (o + 1 < out_sz) {
                out[o++] = *p;
            }
            p++;
            continue;
        }

        p++;
        unsigned cp = 0;
        switch (*p) {
        case '"':  cp = '"';  p++; break;
        case '\\': cp = '\\'; p++; break;
        case '/':  cp = '/';  p++; break;
        case 'n':  cp = '\n'; p++; break;
        case 'r':  cp = '\r'; p++; break;
        case 't':  cp = '\t'; p++; break;
        case 'b':  cp = '\b'; p++; break;
        case 'f':  cp = '\f'; p++; break;
        case 'u':
            cp = parse_hex4(p + 1);
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                const unsigned lo = parse_hex4(p + 2);
                p += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            }
            break;
        default:
            cp = (unsigned char)*p;
            p++;
            break;
        }
        o = utf8_put(out, o, out_sz, cp);
    }

    /* 缓冲区不够时可能停在半个汉字上，把不完整的那一个字节序列去掉 */
    size_t k = o;
    while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) {
        k--;
    }
    if (k > 0) {
        const unsigned char lead = (unsigned char)out[k - 1];
        const size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if (o - (k - 1) < need) {
            o = k - 1;
        }
    }

    out[o] = '\0';
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
    return json_decode_string(find_value(json, key), out, out_sz);
}

static bool json_get_int(const char *json, const char *key, long *out)
{
    const char *p = find_value(json, key);
    if (!p) {
        return false;
    }
    char *end = NULL;
    const long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

/* 检查 MiniMax 的 base_resp.status_code（没有该字段则视为成功） */
static bool api_ok(const char *json)
{
    const char *base = strstr(json, "\"base_resp\"");
    if (!base) {
        return true;
    }
    long code = 0;
    if (!json_get_int(base, "status_code", &code)) {
        return true;
    }
    if (code != 0) {
        char msg[128] = {0};
        json_get_string(base, "status_msg", msg, sizeof(msg));
        ESP_LOGE(TAG, "api error %ld: %s", code, msg);
        return false;
    }
    return true;
}

/* 把用户文本转义成 JSON 字符串内容 */
static bool json_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char tmp[8];
        int n = 0;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            n = 2;
        } else if (*p == '\n') {
            tmp[0] = '\\'; tmp[1] = 'n'; n = 2;
        } else if (*p == '\r') {
            tmp[0] = '\\'; tmp[1] = 'r'; n = 2;
        } else if (*p == '\t') {
            tmp[0] = '\\'; tmp[1] = 't'; n = 2;
        } else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            n = 6;
        } else {
            tmp[0] = (char)*p;
            n = 1;
        }
        if (o + (size_t)n + 1 > out_sz) {
            return false;
        }
        memcpy(out + o, tmp, (size_t)n);
        o += (size_t)n;
    }
    out[o] = '\0';
    return true;
}

/* ---------------- 语音识别 ---------------- */

esp_err_t mm_asr(const uint8_t *wav, size_t wav_len, char *text, size_t text_sz)
{
    if (!mm_ready() || !wav || wav_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    static const char *B = "----espwatch7d91c3f5";
    char head[512];
    const int hl = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"q.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        B, MINIMAX_ASR_MODEL, B);

    char tail[64];
    const int tl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", B);

    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", B);

    const size_t total = (size_t)hl + wav_len + (size_t)tl;
    ESP_LOGI(TAG, "ASR upload %u bytes", (unsigned)wav_len);

    esp_http_client_handle_t c = http_begin("/v1/speech_to_text", ctype, total, "zh");
    if (!c) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    char *resp = NULL;
    size_t rlen = 0;

    if (!http_write_all(c, head, (size_t)hl) ||
        !http_write_all(c, wav, wav_len) ||
        !http_write_all(c, tail, (size_t)tl)) {
        ESP_LOGE(TAG, "upload failed");
        goto done;
    }

    esp_http_client_fetch_headers(c);
    err = http_read_all(c, &resp, &rlen, RESP_MAX_ASR);
    if (err == ESP_OK && !api_ok(resp)) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        if (!json_get_string(resp, "text", text, text_sz) || text[0] == '\0') {
            ESP_LOGW(TAG, "ASR no text: %.200s", resp);
            err = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "ASR: %s", text);
        }
    }

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    mm_free(resp);
    return err;
}

/* ---------------- 大模型对话 ---------------- */

esp_err_t mm_chat(const char *user_text, char *reply, size_t reply_sz)
{
    if (!mm_ready() || !user_text) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t cap = 2048 + strlen(user_text) * 6 + strlen(AI_SYSTEM_PROMPT) * 2;
    char *body = mm_alloc(cap);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char *esc = mm_alloc(cap);
    if (!esc) {
        mm_free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_FAIL;
    if (!json_escape(user_text, esc, cap)) {
        ESP_LOGE(TAG, "question too long");
        goto out;
    }

    int n = snprintf(body, cap,
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"max_completion_tokens\":%d,\"stream\":false,"
        "\"reasoning_effort\":\"%s\"}",
        MINIMAX_CHAT_MODEL, AI_SYSTEM_PROMPT, esc,
        AI_CHAT_MAX_TOKENS, AI_REASONING_EFFORT);

    if (n <= 0 || (size_t)n >= cap) {
        ESP_LOGE(TAG, "request build failed");
        goto out;
    }

    ESP_LOGI(TAG, "chat: %s", user_text);

    esp_http_client_handle_t c = http_begin("/v1/chat/completions", "application/json",
                                            (size_t)n, NULL);
    if (!c) {
        goto out;
    }

    char *resp = NULL;
    size_t rlen = 0;

    if (http_write_all(c, body, (size_t)n)) {
        esp_http_client_fetch_headers(c);
        err = http_read_all(c, &resp, &rlen, RESP_MAX_CHAT);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK) {
        if (!api_ok(resp)) {
            err = ESP_FAIL;
        } else if (!json_get_string(resp, "content", reply, reply_sz) || reply[0] == '\0') {
            ESP_LOGW(TAG, "no content in: %.200s", resp);
            err = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "answer: %s", reply);
        }
    }
    mm_free(resp);

out:
    mm_free(esc);
    mm_free(body);
    return err;
}

/* ---------------- 语音合成 ---------------- */

/* 去掉 Markdown 符号、压缩空白，避免 TTS 念出「星号星号」 */
static void sanitize_for_tts(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    size_t chars = 0;
    bool last_space = false;

    for (const char *p = in; *p && o + 4 < out_sz; p++) {
        char c = *p;

        if (c == '*' || c == '#' || c == '`' || c == '_' || c == '>') {
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        if (c == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }

        /* 只统计 UTF-8 起始字节，保证按「字」截断 */
        if (((unsigned char)c & 0xC0) != 0x80) {
            if (++chars > AI_TTS_MAX_CHARS) {
                break;
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

esp_err_t mm_tts(const char *text, mm_audio_t *out)
{
    if (!mm_ready() || !text || !out) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t clean_sz = (size_t)AI_TTS_MAX_CHARS * 4 + 16;
    char *clean = mm_alloc(clean_sz);
    char *esc = mm_alloc(clean_sz * 2);
    char *body = mm_alloc(1024 + clean_sz * 2);
    if (!clean || !esc || !body) {
        mm_free(clean);
        mm_free(esc);
        mm_free(body);
        return ESP_ERR_NO_MEM;
    }

    sanitize_for_tts(text, clean, clean_sz);
    if (clean[0] == '\0') {
        snprintf(clean, clean_sz, "抱歉，我没有生成出答案。");
    }

    esp_err_t err = ESP_FAIL;
    esp_http_client_handle_t c = NULL;
    char *resp = NULL;
    size_t rlen = 0;

    if (!json_escape(clean, esc, clean_sz * 2)) {
        goto out;
    }

    int n = snprintf(body, 1024 + clean_sz * 2,
        "{\"model\":\"%s\",\"text\":\"%s\",\"stream\":false,\"output_format\":\"hex\","
        "\"voice_setting\":{\"voice_id\":\"%s\",\"speed\":%.2f,\"vol\":1.0,\"pitch\":0},"
        "\"audio_setting\":{\"sample_rate\":%d,\"format\":\"pcm\",\"channel\":1},"
        "\"language_boost\":\"Chinese\"}",
        MINIMAX_TTS_MODEL, esc, MINIMAX_TTS_VOICE, (double)MINIMAX_TTS_SPEED, AI_SAMPLE_RATE);

    if (n <= 0 || (size_t)n >= 1024 + clean_sz * 2) {
        ESP_LOGE(TAG, "tts request build failed");
        goto out;
    }

    ESP_LOGI(TAG, "tts: %s", clean);

    c = http_begin("/v1/t2a_v2", "application/json", (size_t)n, NULL);
    if (!c) {
        goto out;
    }

    if (http_write_all(c, body, (size_t)n)) {
        esp_http_client_fetch_headers(c);
        err = http_read_all(c, &resp, &rlen, RESP_MAX_TTS);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    c = NULL;

    if (err == ESP_OK && !api_ok(resp)) {
        err = ESP_FAIL;
    }

    /* data.audio 是十六进制字符串，就地解码成 PCM */
    if (err == ESP_OK) {
        const char *p = find_value(resp, "audio");
        if (!p || *p != '"') {
            ESP_LOGW(TAG, "no audio in: %.200s", resp);
            err = ESP_FAIL;
        } else {
            const char *hex = p + 1;
            size_t hexlen = 0;
            while (hex[hexlen] && hex[hexlen] != '"') {
                hexlen++;
            }
            if (hex[hexlen] != '"' || hexlen < 8) {
                ESP_LOGW(TAG, "bad audio hex (len %u)", (unsigned)hexlen);
                err = ESP_FAIL;
            } else {
                size_t bytes = hexlen / 2;
                for (size_t i = 0; i < bytes; i++) {
                    const int hi = hex_val(hex[2 * i]);
                    const int lo = hex_val(hex[2 * i + 1]);
                    if (hi < 0 || lo < 0) {
                        err = ESP_FAIL;
                        break;
                    }
                    resp[i] = (char)((hi << 4) | lo);
                }
                if (err == ESP_OK) {
                    out->pcm = (uint8_t *)resp;
                    out->len = bytes;
                    resp = NULL;   /* 所有权转给调用者 */
                    ESP_LOGI(TAG, "tts pcm %u bytes (%u ms)", (unsigned)bytes,
                             (unsigned)(bytes * 1000 / (AI_SAMPLE_RATE * 2)));
                }
            }
        }
    }

out:
    mm_free(resp);
    mm_free(body);
    mm_free(esc);
    mm_free(clean);
    return err;
}
