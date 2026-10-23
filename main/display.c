/*
 * 极简 ST77916 (QSPI) 文本显示实现。
 *
 * 依赖 espressif/esp_lcd_st77916 组件 + 内置 8x16 字模，
 * 不依赖 LVGL。
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
#include "st77916_init_cmds.h"

static const char *TAG = "display";

/* 每次 DMA 传输的行数，只是为了让缓冲区小一点 */
#define LINE_CHUNK          16
#define FLUSH_TIMEOUT_MS    2000

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

/* 等一次 DMA 传输真正结束，避免缓冲区被提前改写 */
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

esp_err_t display_init(void)
{
    /* QSPI 四条数据线 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,                 /* QSPI 没有独立 D/C 线 */
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = s_flush_done,
        .lcd_cmd_bits = 32,                /* QSPI 命令是 32bit */
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };

    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_PCLK,
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .max_transfer_sz = LCD_H_RES * LINE_CHUNK * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "no memory for semaphore");
    io_cfg.user_ctx = s_flush_done;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_cfg, &s_io),
                        TAG, "esp_lcd_new_panel_io_spi failed");

    st77916_vendor_config_t vendor_cfg = {
        /* 组件自带的默认序列是给另一块 ST77916 屏的，必须用微雪 1.85B 的 */
        .init_cmds = waveshare_185b_init_cmds,
        .init_cmds_size = sizeof(waveshare_185b_init_cmds) / sizeof(waveshare_185b_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
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
    ESP_LOGI(TAG, "panel ready: %dx%d (ST77916 QSPI)", LCD_H_RES, LCD_V_RES);
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

void display_draw_text(int x, int y, const char *text, uint8_t scale, uint16_t fg, uint16_t bg)
{
    if (!s_panel || !text) {
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

    const size_t buf_px = (size_t)(x1 - x0) * (size_t)(y1 - y0);
    uint16_t *buf = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGW(TAG, "text buffer alloc failed (%u px)", (unsigned)buf_px);
        return;
    }

    uint16_t *p = buf;
    for (int sy = y0; sy < y1; sy++) {
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

    push_bitmap(x0, y0, x1, y1, buf);
    free(buf);
}
