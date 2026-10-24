/*
 * 计步器：峰值检测 + 步频稳定性校验。
 *
 * 为什么不能「动一下就计一步」：
 *   走路是 1.5~2.5Hz 的周期性上下起伏，而摇表、颠簸是随机的。
 *   所以除了「峰值检测」，还要校验最近几次步子的间隔是否规整。
 *
 * 判定链：
 *   1. 50Hz 采样加速度模值 |a|
 *   2. 一阶低通得到基线（重力 + 缓慢姿态变化）
 *   3. delta = |a| - 基线：> 0.12g 置位，回落到 0.02g 记一次「步子事件」
 *   4. 步子事件进入待确认队列；同时记录它与上一步的间隔
 *   5. 间隔序列满足「均值 340~1000ms 且变异系数 < 25%」时，
 *      把待确认的步子一次性计入 → 稳定步频才加分
 *   6. 超过 2 秒没有新步子，未确认的作废（停下来了）
 *
 * 步数按天保存到 NVS，跨天自动清零。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pedometer.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "imu.h"

static const char *TAG = "pedo";

#define NVS_NAMESPACE       "pedo"
#define NVS_KEY_STEPS       "steps"
#define NVS_KEY_DATE        "date"

#define SAMPLE_INTERVAL_US  20000       /* 50Hz */
#define STEP_MIN_INTERVAL_US 280000     /* 两步最小间隔 */
#define STEP_RISE_G         0.12f       /* 触发阈值 */
#define STEP_FALL_G         0.02f       /* 释放阈值 */
#define SAVE_INTERVAL_US    (60LL * 1000000LL)

#define INTERVAL_HISTORY    8           /* 记录最近 8 次间隔 */
#define RHYTHM_MIN_COUNT    4           /* 至少 4 个间隔才做校验 */
#define RHYTHM_MIN_MS       340         /* 最快 ~2.9 步/秒 */
#define RHYTHM_MAX_MS       1000        /* 最慢 1 步/秒 */
#define RHYTHM_MAX_CV       0.25f       /* 变异系数上限 */
#define PENDING_MAX         5           /* 待确认步子上限 */
#define PENDING_TIMEOUT_US  2000000     /* 2 秒没有新步就作废 */

#define STEP_GOAL           8000

static int s_steps;
static int s_date;                      /* y*10000 + m*100 + d，0 表示未知 */

static float s_baseline;
static bool s_baseline_valid;
static bool s_armed;

static int s_pending;                   /* 已检测到但未确认的步子 */
static int32_t s_intervals[INTERVAL_HISTORY];
static int s_interval_count;

static int64_t s_last_step_us;
static int64_t s_last_sample_us;
static int64_t s_last_save_us;

/* ---------------- NVS ---------------- */

static void storage_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t steps = 0;
    int32_t date = 0;
    if (nvs_get_u32(h, NVS_KEY_STEPS, &steps) == ESP_OK) {
        s_steps = (int)steps;
    }
    if (nvs_get_i32(h, NVS_KEY_DATE, &date) == ESP_OK) {
        s_date = (int)date;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "restored %d steps (date %d)", s_steps, s_date);
}

static void storage_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, NVS_KEY_STEPS, (uint32_t)s_steps);
    nvs_set_i32(h, NVS_KEY_DATE, (int32_t)s_date);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 步频校验 ---------------- */

static void push_interval(int32_t ms)
{
    for (int i = INTERVAL_HISTORY - 1; i > 0; i--) {
        s_intervals[i] = s_intervals[i - 1];
    }
    s_intervals[0] = ms;
    if (s_interval_count < INTERVAL_HISTORY) {
        s_interval_count++;
    }
}

/* 最近几次步子是否构成稳定步频 */
static bool rhythm_is_walking(void)
{
    if (s_interval_count < RHYTHM_MIN_COUNT) {
        return false;
    }

    int32_t sum = 0;
    for (int i = 0; i < s_interval_count; i++) {
        sum += s_intervals[i];
    }
    const int32_t mean = sum / s_interval_count;

    if (mean < RHYTHM_MIN_MS || mean > RHYTHM_MAX_MS) {
        return false;
    }

    int64_t variance = 0;
    for (int i = 0; i < s_interval_count; i++) {
        const int32_t d = s_intervals[i] - mean;
        variance += (int64_t)d * d;
    }
    const float stddev = sqrtf((float)variance / (float)s_interval_count);

    return (stddev / (float)mean) < RHYTHM_MAX_CV;
}

/* ---------------- 对外接口 ---------------- */

void pedometer_init(void)
{
    storage_load();
    s_last_step_us = esp_timer_get_time() - STEP_MIN_INTERVAL_US;
    s_last_sample_us = 0;
    s_last_save_us = esp_timer_get_time();
    s_interval_count = 0;
    s_pending = 0;
    s_armed = false;
    s_baseline_valid = false;
}

void pedometer_set_date(int y, int m, int d)
{
    const int v = y * 10000 + m * 100 + d;
    if (v == s_date) {
        return;
    }

    /* s_date == 0 表示刚开机还没拿到日期，不算跨天 */
    if (s_date != 0) {
        s_steps = 0;
        ESP_LOGI(TAG, "new day %d, steps reset", v);
    }
    s_date = v;
    storage_save();
}

void pedometer_update(void)
{
    if (!imu_present()) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if ((now - s_last_sample_us) < SAMPLE_INTERVAL_US) {
        return;
    }
    s_last_sample_us = now;

    /* 停下来太久：清掉未确认的步子，重新积累步频样本 */
    if (s_pending > 0 && (now - s_last_step_us) > PENDING_TIMEOUT_US) {
        s_pending = 0;
        s_interval_count = 0;
    }

    float ax, ay, az;
    if (imu_read_accel(&ax, &ay, &az) != ESP_OK) {
        return;
    }

    const float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (!s_baseline_valid) {
        s_baseline = mag;
        s_baseline_valid = true;
        return;
    }

    s_baseline += (mag - s_baseline) * 0.10f;
    const float delta = mag - s_baseline;

    if (!s_armed) {
        if (delta > STEP_RISE_G && (now - s_last_step_us) > STEP_MIN_INTERVAL_US) {
            s_armed = true;
        }
        return;
    }

    /* 落回基线：记一次步子事件 */
    if (delta >= STEP_FALL_G) {
        return;
    }

    s_armed = false;
    const int64_t interval = now - s_last_step_us;
    s_last_step_us = now;

    if (interval > (int64_t)PENDING_TIMEOUT_US / 2) {
        /* 断档太久，重新积累步频样本 */
        s_interval_count = 0;
        s_pending = 0;
        return;
    }

    push_interval((int32_t)(interval / 1000));

    if (s_pending < PENDING_MAX) {
        s_pending++;
    }

    if (rhythm_is_walking()) {
        s_steps += s_pending;
        s_pending = 0;
    }

    if ((now - s_last_save_us) > SAVE_INTERVAL_US) {
        s_last_save_us = now;
        storage_save();
    }
}

int pedometer_steps(void)
{
    return s_steps;
}

int pedometer_goal(void)
{
    return STEP_GOAL;
}
