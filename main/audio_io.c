/*
 * 板载音频：ES8311（喇叭）+ ES7210（麦克风），共用一组 I2S 时钟。
 *
 * 硬件连线见 board_config.h：MCLK=2 BCLK=48 LRCK=38 DIN=39 DOUT=47 PA=9。
 * 两个 codec 都挂在 board_i2c 上（ES8311 0x30、ES7210 0x80）。
 *
 * 设计取舍：
 *   - 一路 I2S 全双工（TX 是时钟主机），两个 codec 共用同一个 data_if；
 *   - 采样固定 16kHz / 16bit / 双声道（ES7210 双麦、ES8311 单声道），
 *     录音取左声道，播放把单声道复制到两个声道；
 *   - 不用时把 I2S 通道关掉并关功放，省电。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "audio_io.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "ai_config.h"
#include "board_config.h"
#include "board_i2c.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_data_if_t *s_data_if;

static esp_codec_dev_handle_t s_out;   /* ES8311 */
static esp_codec_dev_handle_t s_in;    /* ES7210 */

static bool s_ready;
static bool s_io_on;
static bool s_recording;

/* 播/录的中转缓冲（内部 RAM，DMA 友好） */
static int16_t s_stage[512 * 2];

esp_err_t audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 功放使能脚，默认关 */
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << AUDIO_PIN_PA,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa);
    gpio_set_level(AUDIO_PIN_PA, 0);

    /* ---- I2S 全双工 ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AI_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_PIN_MCLK,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_LRCK,
            .dout = AUDIO_PIN_DOUT,
            .din = AUDIO_PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    /* TX 先初始化：它是时钟主机 */
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std mode: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 数据接口 / 控制接口 ---- */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
        .clk_src = 0,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!s_data_if) {
        ESP_LOGE(TAG, "i2s data if failed");
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = TOUCH_I2C_PORT,
        .addr = AUDIO_I2C_ADDR_ES8311,   /* 8bit 写法，驱动内部 >>1 */
        .bus_handle = board_i2c_handle(),
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    const audio_codec_ctrl_if_t *ctrl8311 = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl8311) {
        ESP_LOGE(TAG, "es8311 ctrl if failed");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl8311,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,                     /* 功放自己控制 */
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f, .pa_gain = 0.0f },
        .mclk_div = 256,
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);
    if (!es8311) {
        ESP_LOGE(TAG, "es8311 codec new failed (I2C?)");
        return ESP_FAIL;
    }

    i2c_cfg.addr = AUDIO_I2C_ADDR_ES7210;
    const audio_codec_ctrl_if_t *ctrl7210 = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl7210) {
        ESP_LOGE(TAG, "es7210 ctrl if failed");
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctrl7210,
        .master_mode = false,
        .mic_selected = AI_MIC_SELECT,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    const audio_codec_if_t *es7210 = es7210_codec_new(&es7210_cfg);
    if (!es7210) {
        ESP_LOGE(TAG, "es7210 codec new failed (I2C?)");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311,
        .data_if = s_data_if,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210,
        .data_if = s_data_if,
    };
    s_out = esp_codec_dev_new(&out_cfg);
    s_in = esp_codec_dev_new(&in_cfg);
    if (!s_out || !s_in) {
        ESP_LOGE(TAG, "codec dev new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = AI_SAMPLE_RATE,
        .mclk_multiple = 256,
    };

    int ret = esp_codec_dev_open(s_out, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "es8311 open: %d", ret);
        return ESP_FAIL;
    }
    ret = esp_codec_dev_open(s_in, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "es7210 open: %d", ret);
        return ESP_FAIL;
    }

    esp_codec_dev_set_out_vol(s_out, AI_SPEAKER_VOLUME);
    esp_codec_dev_set_in_gain(s_in, AI_MIC_GAIN_DB);

    /* codec 已配好，先把时钟停掉省电 */
    i2s_channel_disable(s_rx);
    i2s_channel_disable(s_tx);
    s_io_on = false;

    s_ready = true;
    ESP_LOGI(TAG, "ready (16kHz/16bit, ES8311 + ES7210)");
    return ESP_OK;
}

bool audio_ready(void)
{
    return s_ready;
}

static void audio_io_on(void)
{
    if (s_io_on) {
        return;
    }
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    s_io_on = true;
}

static void audio_io_off(void)
{
    if (!s_io_on || s_recording) {
        return;
    }
    i2s_channel_disable(s_rx);
    i2s_channel_disable(s_tx);
    s_io_on = false;
}

esp_err_t audio_record_begin(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_io_on();
    s_recording = true;

    /* 丢掉开头两帧，避免拿到上一轮的残留数据 */
    for (int i = 0; i < 2; i++) {
        esp_codec_dev_read(s_in, s_stage, sizeof(s_stage));
    }
    return ESP_OK;
}

int audio_record_read(int16_t *dst, int max_frames)
{
    if (!s_ready || !s_recording || !dst || max_frames <= 0) {
        return 0;
    }
    const int bytes = max_frames * 2 * (int)sizeof(int16_t);
    if (esp_codec_dev_read(s_in, dst, bytes) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    return max_frames;
}

esp_err_t audio_record_end(void)
{
    if (!s_recording) {
        return ESP_OK;
    }
    s_recording = false;
    audio_io_off();
    return ESP_OK;
}

esp_err_t audio_play(const int16_t *pcm, size_t samples, volatile bool *cancel)
{
    if (!s_ready || !pcm || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_play_begin();

    size_t done = 0;
    while (done < samples) {
        if (cancel && *cancel) {
            break;
        }
        const size_t n = (samples - done > 512) ? 512 : (samples - done);
        if (audio_play_write(pcm + done, n) != ESP_OK) {
            break;
        }
        done += n;
    }

    audio_play_end();
    return ESP_OK;
}

esp_err_t audio_play_begin(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_io_on();
    gpio_set_level(AUDIO_PIN_PA, 1);
    vTaskDelay(pdMS_TO_TICKS(20));   /* 等功放上电，避免咬字 */
    return ESP_OK;
}

esp_err_t audio_play_write(const int16_t *pcm, size_t samples)
{
    if (!s_ready || !pcm || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t done = 0;
    while (done < samples) {
        const size_t n = (samples - done > 512) ? 512 : (samples - done);
        for (size_t i = 0; i < n; i++) {
            s_stage[2 * i] = pcm[done + i];
            s_stage[2 * i + 1] = pcm[done + i];
        }
        if (esp_codec_dev_write(s_out, s_stage, (int)(n * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "i2s write failed at %u/%u", (unsigned)done, (unsigned)samples);
            return ESP_FAIL;
        }
        done += n;
    }
    return ESP_OK;
}

esp_err_t audio_play_end(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 等 DMA 把最后一帧送出去 */
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(AUDIO_PIN_PA, 0);
    audio_io_off();
    return ESP_OK;
}

void audio_pa_enable(bool on)
{
    if (!s_ready) {
        return;
    }
    gpio_set_level(AUDIO_PIN_PA, on ? 1 : 0);
}
