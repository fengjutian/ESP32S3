/*
 * 极简 ST77916 (QSPI) 文本显示实现。
 *
 * 初始化流程照抄 Waveshare 官方 BSP（读 0x04 判定屏幕版本 → 选初始化序列），
 * 文本绘制用内置 8x16 点阵字模，不依赖 LVGL。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"

#include "display.h"
#include "display_config.h"
#include "font8x16.h"
#include "font16x16_zh.h"
#include "st77916_init_cmds.h"

static const char *TAG = "display";

/* 每次 DMA 传输的行数 */
#define LINE_CHUNK          16
#define FLUSH_TIMEOUT_MS    2000

#define LCD_OPCODE_READ_CMD  (0x0B)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_strip;   /* LINE_CHUNK 行 × 屏宽 的像素缓冲 */

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

static void push_bitmap(int x0, int y0, int x1, int y1, const void *buf)
{
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

    display_backlight(true);
    ESP_LOGI(TAG, "panel ready: %dx%d (ST77916 QSPI, %s)", LCD_H_RES, LCD_V_RES, ver_name);
    return ESP_OK;
}

void display_backlight(bool on)
{
#if (LCD_PIN_BL >= 0)
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(LCD_PIN_BL, on ? 1 : 0);
#else
    (void)on;
#endif
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
