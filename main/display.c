/*
 * 极简 ST77916 (QSPI) 文本显示实现。
 *
 * 初始化流程照抄 Waveshare 官方 BSP（读 0x04 判定屏幕版本 → 选初始化序列），
 * 文本绘制用内置 8x16 点阵字模，不依赖 LVGL。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"

#include "display.h"
#include "board_config.h"
#include "font8x16.h"
#include "font16x16_zh.h"
#include "st77916_init_cmds.h"

static const char *TAG = "display";

/* 每次 DMA 传输的行数 */
#define LINE_CHUNK          64
#define FLUSH_TIMEOUT_MS    2000

#define LCD_OPCODE_READ_CMD  (0x0B)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_strip;   /* LINE_CHUNK 行 × 屏宽 的像素缓冲 */

/* 整页离屏合成：切页时先画到 PSRAM 再整页推屏，避免清屏黑闪。
 * 没有 PSRAM 时 s_fb 为 NULL，所有绘制直接走屏幕。 */
static uint16_t *s_fb;
static bool s_compose;
static int s_frame_depth;
static TaskHandle_t s_frame_owner;       /* 正在合成整页的任务，其它任务的局部刷新照旧直接刷屏 */
static SemaphoreHandle_t s_frame_lock;   /* 整页合成互斥（可重入），避免两个任务同时画一帧 */
static portMUX_TYPE s_frame_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t backlight_init(void);

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static void wait_flush(void)
{
    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(FLUSH_TIMEOUT_MS));
    }
}

static void push_bitmap(int x0, int y0, int x1, int y1, const void *buf_in)
{
    /* 离屏合成阶段：只写帧缓冲，等 display_frame_end() 再整页推屏。
     * 这里不做字节交换，推屏时统一交换一次。
     * 只有发起合成的任务才写帧缓冲；别的任务此刻的局部刷新仍然直接刷屏，
     * 不会被误画进这一帧里。 */
    if (s_compose && s_fb && xTaskGetCurrentTaskHandle() == s_frame_owner) {
        uint16_t *src = (uint16_t *)buf_in;
        const int w = x1 - x0;
        const int h = y1 - y0;
        for (int r = 0; r < h; r++) {
            memcpy(&s_fb[(size_t)(y0 + r) * LCD_H_RES + x0],
                   src + (size_t)r * (size_t)w,
                   (size_t)w * sizeof(uint16_t));
        }
        return;
    }

    /* 面板初始化序列里的 0xC8 (RAMCTL) 固定了字节序，data_endian 会被覆盖，
     * 所以在这里统一做一次字节交换。所有绘制路径都经过这里。 */
    uint16_t *buf = (uint16_t *)buf_in;
    const size_t n = (size_t)(x1 - x0) * (size_t)(y1 - y0);
    for (size_t i = 0; i < n; i++) {
        const uint16_t c = buf[i];
        buf[i] = (uint16_t)((c >> 8) | (c << 8));
    }

    xSemaphoreTake(s_flush_done, 0);   /* 清掉可能残留的旧信号 */
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, buf);
    wait_flush();
}

/* 按寄存器 0x04 的值选初始化序列，和厂商 BSP 的规则一致 */
static void select_init_sequence(const uint8_t id[4], bool id_valid,
                                 const st77916_lcd_init_cmd_t **cmds,
                                 uint16_t *count, const char **name)
{
#if (LCD_INIT_VERSION == 1)
    *cmds = waveshare_185b_init_v1;
    *count = sizeof(waveshare_185b_init_v1) / sizeof(waveshare_185b_init_v1[0]);
    *name = "v1 (forced)";
#elif (LCD_INIT_VERSION == 2)
    *cmds = waveshare_185b_init_v2;
    *count = sizeof(waveshare_185b_init_v2) / sizeof(waveshare_185b_init_v2[0]);
    *name = "v2 (forced)";
#else
    if (id_valid && id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        *cmds = waveshare_185b_init_v2;
        *count = sizeof(waveshare_185b_init_v2) / sizeof(waveshare_185b_init_v2[0]);
        *name = "v2";
    } else {
        *cmds = waveshare_185b_init_v1;
        *count = sizeof(waveshare_185b_init_v1) / sizeof(waveshare_185b_init_v1[0]);
        *name = id_valid ? "v1" : "v1 (id read failed)";
    }
#endif
}

esp_err_t display_init(void)
{
    /* 1. 硬复位（厂商 BSP 在初始化总线之前先拉一次） */
#if (LCD_PIN_RST >= 0)
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << LCD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /* 2. QSPI 总线 */
    const spi_bus_config_t bus_cfg = {
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .sclk_io_num = LCD_PIN_PCLK,
        .max_transfer_sz = LCD_H_RES * LINE_CHUNK * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "no memory for semaphore");

    s_frame_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_frame_lock) {
        ESP_LOGW(TAG, "no memory for frame lock, page framing disabled");
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,                 /* QSPI 没有独立 D/C 线 */
        .spi_mode = 0,
        .pclk_hz = 3 * 1000 * 1000,        /* 先低速读屏版本 */
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = s_flush_done,
        .lcd_cmd_bits = 32,                /* QSPI 命令是 32bit */
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_cfg, &s_io),
                        TAG, "esp_lcd_new_panel_io_spi failed");

    /* 3. 读寄存器 0x04 判断屏幕版本 */
    uint8_t id[4] = {0};
    const int read_cmd = (LCD_OPCODE_READ_CMD << 24) | (0x04 << 8);
    esp_err_t rd = esp_lcd_panel_io_rx_param(s_io, read_cmd, id, sizeof(id));
    const bool id_valid = (rd == ESP_OK);
    if (id_valid) {
        ESP_LOGI(TAG, "panel reg 0x04 = %02X %02X %02X %02X", id[0], id[1], id[2], id[3]);
    } else {
        ESP_LOGW(TAG, "read reg 0x04 failed (%s), falling back to v1", esp_err_to_name(rd));
    }

    /* 4. 切到全速 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(s_io), TAG, "panel io del failed");
    s_io = NULL;
    io_cfg.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_cfg, &s_io),
                        TAG, "esp_lcd_new_panel_io_spi (fast) failed");

    /* 5. 选初始化序列并建面板 */
    const st77916_lcd_init_cmd_t *cmds = NULL;
    uint16_t cmd_count = 0;
    const char *ver_name = "?";
    select_init_sequence(id, id_valid, &cmds, &cmd_count, &ver_name);
    ESP_LOGI(TAG, "using init sequence %s (%u commands)", ver_name, (unsigned)cmd_count);

    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = cmds,
        .init_cmds_size = cmd_count,
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,  /* 字节序：默认 BIG 会导致颜色整体反色 */
        .bits_per_pixel = 16,
        .flags = {
            .reset_active_high = 0,
        },
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel),
                        TAG, "esp_lcd_new_panel_st77916 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
#if LCD_INVERT_COLOR
    esp_lcd_panel_invert_color(s_panel, true);
#endif
#if LCD_SWAP_XY
    esp_lcd_panel_swap_xy(s_panel, true);
#endif
#if (LCD_MIRROR_X || LCD_MIRROR_Y)
    esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y);
#endif
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_strip = heap_caps_malloc((size_t)LCD_H_RES * LINE_CHUNK * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_strip, ESP_ERR_NO_MEM, TAG, "no memory for strip buffer");

    /* 整页帧缓冲放 PSRAM：360×360×2 ≈ 253 KB。分配失败就退回直接刷屏，
     * 功能不受影响，只是切页仍会有黑闪。 */
    s_fb = heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb) {
        ESP_LOGI(TAG, "page framebuffer: %u KB in PSRAM",
                 (unsigned)((size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t) / 1024));
    } else {
        ESP_LOGW(TAG, "no PSRAM for page framebuffer, page switches draw directly");
    }

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");
    display_set_backlight(100);
    ESP_LOGI(TAG, "panel ready: %dx%d (ST77916 QSPI, %s)", LCD_H_RES, LCD_V_RES, ver_name);
    return ESP_OK;
}

/* ---------------- 背光（LEDC PWM） ---------------- */

#define BL_MODE         LEDC_LOW_SPEED_MODE
#define BL_TIMER        LEDC_TIMER_0
#define BL_CHANNEL      LEDC_CHANNEL_0
#define BL_DUTY_MAX     1023   /* 10 bit */

static bool s_bl_ready;

static esp_err_t backlight_init(void)
{
#if (LCD_PIN_BL >= 0)
    ledc_timer_config_t tcfg = {
        .speed_mode = BL_MODE,
        .timer_num = BL_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc timer config failed");

    ledc_channel_config_t ccfg = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc channel config failed");

    s_bl_ready = true;
#endif
    return ESP_OK;
}

void display_set_backlight(int percent)
{
#if (LCD_PIN_BL >= 0)
    if (!s_bl_ready) {
        return;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    const uint32_t duty = (uint32_t)((BL_DUTY_MAX * percent) / 100);
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
#else
    (void)percent;
#endif
}

void display_backlight(bool on)
{
    display_set_backlight(on ? 100 : 0);
}

/* 用纯色填充矩形，按 LINE_CHUNK 行分批 DMA */
void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel || !s_strip || w <= 0 || h <= 0) {
        return;
    }

    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    const size_t row_px = (size_t)(x1 - x0);
    for (int yy = y0; yy < y1; yy += LINE_CHUNK) {
        const int lines = ((y1 - yy) < LINE_CHUNK) ? (y1 - yy) : LINE_CHUNK;
        const size_t n = row_px * (size_t)lines;
        for (size_t i = 0; i < n; i++) {
            s_strip[i] = color;
        }
        push_bitmap(x0, yy, x1, yy + lines, s_strip);
    }
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

void display_frame_begin(void)
{
    if (!s_fb || !s_frame_lock) {
        return;
    }

    /* 等到上一帧画完再开始，免得两个任务往同一块帧缓冲里交错写 */
    xSemaphoreTakeRecursive(s_frame_lock, portMAX_DELAY);

    portENTER_CRITICAL(&s_frame_mux);
    if (s_frame_depth == 0) {
        s_frame_owner = xTaskGetCurrentTaskHandle();
    }
    s_frame_depth++;
    s_compose = true;
    portEXIT_CRITICAL(&s_frame_mux);
}

void display_frame_end(void)
{
    if (!s_fb || !s_frame_lock) {
        return;
    }

    bool flush = false;
    portENTER_CRITICAL(&s_frame_mux);
    if (s_frame_depth > 0 && --s_frame_depth == 0) {
        s_compose = false;
        s_frame_owner = NULL;
        flush = true;
    }
    portEXIT_CRITICAL(&s_frame_mux);

    if (flush) {
        for (int y = 0; y < LCD_V_RES; y += LINE_CHUNK) {
            const int lines = ((LCD_V_RES - y) < LINE_CHUNK) ? (LCD_V_RES - y) : LINE_CHUNK;
            memcpy(s_strip, &s_fb[(size_t)y * LCD_H_RES],
                   (size_t)LCD_H_RES * (size_t)lines * sizeof(uint16_t));
            push_bitmap(0, y, LCD_H_RES, y + lines, s_strip);
        }
    }

    xSemaphoreGiveRecursive(s_frame_lock);
}

int display_text_width(const char *text, uint8_t scale)
{
    if (!text || scale == 0) {
        return 0;
    }
    return (int)strlen(text) * FONT8X16_WIDTH * scale;
}

/* 文本按 LINE_CHUNK 行分批渲染，避免单次传输超过 max_transfer_sz */
void display_draw_text(int x, int y, const char *text, uint8_t scale, uint16_t fg, uint16_t bg)
{
    if (!s_panel || !s_strip || !text) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }

    const int len = (int)strlen(text);
    if (len <= 0) {
        return;
    }

    const int glyph_w = FONT8X16_WIDTH * scale;
    const int glyph_h = FONT8X16_HEIGHT * scale;

    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + len * glyph_w;
    int y1 = y + glyph_h;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    const size_t row_px = (size_t)(x1 - x0);
    for (int yy = y0; yy < y1; yy += LINE_CHUNK) {
        const int lines = ((y1 - yy) < LINE_CHUNK) ? (y1 - yy) : LINE_CHUNK;
        uint16_t *p = s_strip;

        for (int sy = yy; sy < yy + lines; sy++) {
            const int gy = (sy - y) / scale;
            for (int sx = x0; sx < x1; sx++) {
                const int idx = (sx - x) / glyph_w;
                const int gx = ((sx - x) % glyph_w) / scale;
                bool on = false;

                if (idx >= 0 && idx < len) {
                    const int code = (unsigned char)text[idx];
                    if (code >= FONT8X16_FIRST_CHAR && code <= FONT8X16_LAST_CHAR) {
                        const uint8_t bits = font8x16[code - FONT8X16_FIRST_CHAR][gy];
                        on = ((bits >> (FONT8X16_WIDTH - 1 - gx)) & 0x01) != 0;
                    }
                }
                *p++ = on ? fg : bg;
            }
        }
        push_bitmap(x0, yy, x1, yy + lines, s_strip);
    }
    (void)row_px;
}

/* ------------------------------------------------------------------ */
/* UTF-8 文本：ASCII 用 8x16 字模，中文用 16x16 点阵                    */
/* ------------------------------------------------------------------ */

static const uint8_t *zh_lookup(uint16_t code)
{
    int lo = 0;
    int hi = ZH_FONT_COUNT - 1;

    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (zh_font[mid].code == code) {
            return zh_font[mid].bitmap;
        }
        if (zh_font[mid].code < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* 返回消耗的字节数，cp 为解码出的 Unicode 码位 */
static int utf8_decode(const uint8_t *p, uint32_t *cp)
{
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) |
              ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

/* 把 w x h 的位图画到屏幕，每行 stride 字节，MSB 在左 */
static void blit_glyph(int x, int y, const uint8_t *rows, int w, int h,
                       uint8_t scale, uint16_t fg, uint16_t bg)
{
    if (!s_panel || !s_strip || !rows) {
        return;
    }

    const int stride = (w + 7) / 8;
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + w * scale;
    int y1 = y + h * scale;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy += LINE_CHUNK) {
        const int lines = ((y1 - yy) < LINE_CHUNK) ? (y1 - yy) : LINE_CHUNK;
        uint16_t *p = s_strip;

        for (int sy = yy; sy < yy + lines; sy++) {
            const int gy = (sy - y) / scale;
            for (int sx = x0; sx < x1; sx++) {
                const int gx = (sx - x) / scale;
                const uint8_t bits = rows[gy * stride + (gx >> 3)];
                const bool on = ((bits >> (7 - (gx & 7))) & 0x01) != 0;
                *p++ = on ? fg : bg;
            }
        }
        push_bitmap(x0, yy, x1, yy + lines, s_strip);
    }
}

void display_draw_text_utf8(int x, int y, const char *text, uint8_t scale, uint16_t fg, uint16_t bg)
{
    if (!s_panel || !s_strip || !text) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }

    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        uint32_t cp = 0;
        p += utf8_decode(p, &cp);

        const uint8_t *bmp;
        int w = 8;

        if (cp < 0x80) {
            bmp = (cp >= FONT8X16_FIRST_CHAR && cp <= FONT8X16_LAST_CHAR)
                      ? font8x16[cp - FONT8X16_FIRST_CHAR]
                      : font8x16['?' - FONT8X16_FIRST_CHAR];
        } else {
            bmp = (cp <= 0xFFFF) ? zh_lookup((uint16_t)cp) : NULL;
            if (bmp) {
                w = 16;
            } else {
                bmp = font8x16['?' - FONT8X16_FIRST_CHAR];
            }
        }

        blit_glyph(x, y, bmp, w, FONT8X16_HEIGHT, scale, fg, bg);
        x += w * scale;
        if (x >= LCD_H_RES) {
            break;
        }
    }
}

int display_text_width_utf8(const char *text, uint8_t scale)
{
    if (!text || scale == 0) {
        return 0;
    }

    int w = 0;
    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        uint32_t cp = 0;
        p += utf8_decode(p, &cp);
        w += ((cp < 0x80) ? FONT8X16_WIDTH : 16) * scale;
    }
    return w;
}

void display_draw_ring(int cx, int cy, int radius, int thickness,                       int start_deg, int end_deg,
                       uint16_t fg, uint16_t track, uint16_t bg)
{
    if (!s_panel || !s_strip || radius <= 0 || thickness <= 0) {
        return;
    }

    const int r_in = (radius - thickness > 0) ? (radius - thickness) : 0;
    const int r_out2 = radius * radius;
    const int r_in2 = r_in * r_in;

    int x0 = cx - radius;
    int x1 = cx + radius + 1;
    int y0 = cy - radius;
    int y1 = cy + radius + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy += LINE_CHUNK) {
        const int lines = ((y1 - yy) < LINE_CHUNK) ? (y1 - yy) : LINE_CHUNK;
        uint16_t *p = s_strip;

        for (int sy = yy; sy < yy + lines; sy++) {
            const int dy = sy - cy;
            for (int sx = x0; sx < x1; sx++) {
                const int dx = sx - cx;
                const int d2 = dx * dx + dy * dy;
                uint16_t color;

                if (d2 > r_out2 || d2 < r_in2) {
                    color = bg;
                } else {
                    float deg = atan2f((float)dx, (float)(-dy)) * 57.29578f;
                    if (deg < 0.0f) {
                        deg += 360.0f;
                    }
                    color = (deg >= (float)start_deg && deg < (float)end_deg) ? fg : track;
                }
                *p++ = color;
            }
        }
        push_bitmap(x0, yy, x1, yy + lines, s_strip);
    }
}

void display_draw_arc(int cx, int cy, int radius, int thickness,
                      int a0, int a1, uint16_t fg, uint16_t track, uint16_t bg)
{
    if (!s_panel || !s_strip || radius <= 0 || thickness <= 0 || a1 <= a0) {
        return;
    }

    const int r_in = (radius - thickness > 0) ? (radius - thickness) : 0;
    const int r_out2 = radius * radius;
    const int r_in2 = r_in * r_in;

    /* 扇形包围盒：只沿角度采样外圈端点（不要把圆心算进去，否则包围盒会
     * 撑成「圆心→边缘」的大矩形，重绘时会把圆环其余部分和文字一起涂掉） */
    int minx = INT32_MAX, maxx = INT32_MIN;
    int miny = INT32_MAX, maxy = INT32_MIN;
    for (int a = a0; a <= a1; a += 2) {
        const float rad = (float)a * 3.14159265f / 180.0f;
        const int x = cx + (int)(sinf(rad) * (float)radius);
        const int y = cy - (int)(cosf(rad) * (float)radius);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    /* 再按环宽外扩 */
    minx -= thickness;
    maxx += thickness;
    miny -= thickness;
    maxy += thickness;

    int x0 = (minx < 0) ? 0 : minx;
    int y0 = (miny < 0) ? 0 : miny;
    int x1 = (maxx + 1 > LCD_H_RES) ? LCD_H_RES : maxx + 1;
    int y1 = (maxy + 1 > LCD_V_RES) ? LCD_V_RES : maxy + 1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy += LINE_CHUNK) {
        const int lines = ((y1 - yy) < LINE_CHUNK) ? (y1 - yy) : LINE_CHUNK;
        uint16_t *p = s_strip;

        for (int sy = yy; sy < yy + lines; sy++) {
            const int dy = sy - cy;
            for (int sx = x0; sx < x1; sx++) {
                const int dx = sx - cx;
                const int d2 = dx * dx + dy * dy;
                uint16_t color = bg;

                if (d2 <= r_out2 && d2 >= r_in2) {
                    float deg = atan2f((float)dx, (float)(-dy)) * 57.29578f;
                    if (deg < 0.0f) {
                        deg += 360.0f;
                    }
                    color = (deg >= (float)a0 && deg < (float)a1) ? fg : track;
                }
                *p++ = color;
            }
        }
        push_bitmap(x0, yy, x1, yy + lines, s_strip);
    }
}

uint16_t *display_band_buffer(void){
    return s_strip;
}

void display_band_push(int x0, int y, int w, int lines)
{
    if (!s_panel || !s_strip || w <= 0 || lines <= 0) {
        return;
    }
    if (x0 < 0 || y < 0 || x0 + w > LCD_H_RES || y + lines > LCD_V_RES ||
        w > LCD_H_RES || lines > LINE_CHUNK) {
        return;
    }
    push_bitmap(x0, y, x0 + w, y + lines, s_strip);
}
