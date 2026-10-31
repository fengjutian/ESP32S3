/*
 * 弹球动画表盘实现。
 *
 * 渲染策略和贪吃蛇一致：不做整屏刷新，每帧只重绘「球的旧位置 ∪ 新位置
 * ∪ 拖尾残影 ∪ 涟漪」这些小块区域。一个区域里可能有多个球（互相压住），
 * 所以顺序是：先重建背景，再画涟漪，最后画拖尾和球；相交的脏矩形先合并，
 * 保证重叠区域不会被擦了又画、留下半只球。
 *
 * 背景不是纯黑：最外面有一圈装饰环。脏矩形的角可能伸到环上，所以重绘时
 * 按半径逐像素重建背景（半径 166~172px 画环色，其余画黑），环不会被
 * 擦出缺口。
 *
 * 特效：
 *   拖尾  —— 每个球保留最近 3 帧的位置，由深到浅画成残影；
 *   涟漪  —— 球球相撞时在接触点扩散一圈亮环，约 0.25 秒消失。
 * 两者都只是「多画几个圆」，物理不受影响。
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "anim.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_random.h"
#include "esp_timer.h"

#include "display.h"
#include "imu.h"
#include "ui.h"

#define BALL_COUNT    8
#define BALL_R_MIN    8
#define BALL_R_MAX    15

#define ARENA_EDGE    162.0f    /* 球边缘到圆心的最大距离（px） */
#define RING_COLOR    0x2104    /* 外圈装饰环颜色 */
#define RING_IN2      ((UI_RING_R - UI_RING_W) * (UI_RING_R - UI_RING_W))
#define RING_OUT2     (UI_RING_R * UI_RING_R)
#define ARENA_CLIP2   ((UI_RING_R - UI_RING_W - 1) * (UI_RING_R - UI_RING_W - 1))
#define GRAVITY_GAIN   1500.0f  /* 1g 倾斜对应的加速度（px/s²） */
#define GRAVITY_DEAD   0.12f    /* 小于这个就当平放，球不会慢慢漂向一边 */
#define GRAVITY_LPF    0.30f    /* 低通，滤掉手抖 */
#define GRAVITY_SIGN_X (-1.0f)  /* 传感器方向反了就把它改成 +1 */
#define GRAVITY_SIGN_Y (-1.0f)
#define IMU_EVERY_N    2        /* 每 2 帧读一次 IMU（约 15Hz），省 I2C 带宽 */
#define WALL_BOUNCE   1.0f      /* 撞外圈的能量保留：完全弹性，球永远不会停 */
#define BALL_BOUNCE   1.0f      /* 球球碰撞：同样按完全弹性处理 */
#define MIN_SPEED     60.0f     /* 兜底：速度低到快停住时补一脚 */
#define MAX_SPEED     420.0f

#define DT_MAX        0.05f     /* 单帧最多推进 50ms（切页回来时防跳变） */
#define SUBSTEP       0.016f    /* 物理子步长，约 60Hz */
#define FRAME_US      33000     /* 约 30fps，再快 SPI 推屏也吃不下 */

#define BALL_GHOST    2         /* 脏矩形比球再外扩几像素 */
#define TRAIL_LEN     5         /* 拖尾保留几帧 */

#define RIPPLE_MAX    10        /* 同时存在的涟漪上限 */
#define RIPPLE_LIFE   7         /* 涟漪存活帧数（30fps 下约 0.23s） */
#define RIPPLE_STEP   3         /* 每帧半径增长（px） */
#define RIPPLE_R0     5         /* 起始半径 */
#define RIPPLE_BUDGET 3         /* 每帧最多生成几个，碰撞密集时不刷屏 */
#define RIPPLE_MIN_V  45.0f     /* 撞击强度低于这个值就不冒涟漪 */

typedef struct {
    float x, y;
    float vx, vy;
    int r;
    int ix, iy;      /* 本帧整数圆心 */
    int ox, oy;      /* 上帧整数圆心 */
    int tx[TRAIL_LEN + 1], ty[TRAIL_LEN + 1];   /* 拖尾位置，[0] 最近；
                                                 * 最后一个槽只用于“擦除覆盖”，不绘制 */
    uint16_t color;
    uint16_t hi;     /* 高光色 */
    uint16_t tc[TRAIL_LEN];             /* 拖尾颜色，由近到远变暗 */
} ball_t;

typedef struct {
    int x, y;
    int age;         /* -1 = 空闲 */
    uint16_t color;
} ripple_t;

typedef struct {
    int x0, y0, x1, y1;   /* x1 / y1 为开区间 */
} rect_t;

static const uint16_t k_colors[BALL_COUNT] = {
    COLOR_RED, COLOR_GREEN, COLOR_CYAN, COLOR_ORANGE,
    COLOR_MAGENTA, COLOR_YELLOW, 0x07EF, COLOR_WHITE,
};

static ball_t s_ball[BALL_COUNT];
static ripple_t s_ripple[RIPPLE_MAX];
static int s_ripple_budget;
static float s_gx;               /* 屏幕系重力加速度（px/s²） */
static float s_gy;
static int s_imu_skip;
static bool s_paused;
static int64_t s_last_us;
static int64_t s_next_us;
static bool s_started;

/* ---------------- 小工具 ---------------- */

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)(esp_random() & 0xFFFFu) / 65535.0f);
}

/* pct > 0 向白色混合，pct < 0 向黑色压暗 */
static uint16_t shade(uint16_t c, int pct)
{
    int r = (c >> 11) & 0x1F;
    int g = (c >> 5) & 0x3F;
    int b = c & 0x1F;

    if (pct >= 0) {
        r += (31 - r) * pct / 100;
        g += (63 - g) * pct / 100;
        b += (31 - b) * pct / 100;
    } else {
        r = r * (100 + pct) / 100;
        g = g * (100 + pct) / 100;
        b = b * (100 + pct) / 100;
    }
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static float speed_of(const ball_t *b)
{
    return sqrtf(b->vx * b->vx + b->vy * b->vy);
}

/* 倾斜重力：平放时加速度基本只有 z 轴（a≈(0,0,1g)），x/y 分量落在死区内，
 * 等价于没有重力；倾斜手表时把 x/y 投影到屏幕，球就往低处滚。 */
static void gravity_update(void)
{
    if (s_imu_skip > 0) {
        s_imu_skip--;
        return;
    }
    s_imu_skip = IMU_EVERY_N - 1;

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (imu_read_accel(&ax, &ay, &az) != ESP_OK) {
        return;
    }

    ax *= GRAVITY_SIGN_X;
    ay *= GRAVITY_SIGN_Y;

    if (sqrtf(ax * ax + ay * ay) < GRAVITY_DEAD) {
        ax = 0.0f;
        ay = 0.0f;
    }

    s_gx += (ax * GRAVITY_GAIN - s_gx) * GRAVITY_LPF;
    s_gy += (ay * GRAVITY_GAIN - s_gy) * GRAVITY_LPF;
}

/* ---------------- 绘制 ---------------- */

static void ripple_paint(uint16_t *buf, int w, int y0, int lines, int rx0, const ripple_t *rp)
{
    const int r = RIPPLE_R0 + rp->age * RIPPLE_STEP;
    const int in = (r > 3) ? (r - 3) : 1;
    const uint16_t c = shade(rp->color, -(rp->age * 9));
    const int in2 = in * in;
    const int out2 = r * r;

    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > out2 || d2 < in2) {
                continue;
            }
            /* 涟漪贴到边上时裁在环内侧，别糊到装饰环上 */
            const int cx = rp->x + dx - UI_RING_CX;
            const int cy = rp->y + dy - UI_RING_CY;
            if (cx * cx + cy * cy > ARENA_CLIP2) {
                continue;
            }
            ui_px(buf, w, y0, lines, rx0, rp->x + dx, rp->y + dy, c);
        }
    }
}

/* 重绘一个矩形：重建背景 → 涟漪 → 拖尾 → 球 → 分批推屏 */
static void rect_draw(const rect_t *in)
{
    rect_t rc = *in;
    if (rc.x0 < 0) rc.x0 = 0;
    if (rc.y0 < 0) rc.y0 = 0;
    if (rc.x1 > LCD_H_RES) rc.x1 = LCD_H_RES;
    if (rc.y1 > LCD_V_RES) rc.y1 = LCD_V_RES;
    if (rc.x0 >= rc.x1 || rc.y0 >= rc.y1) {
        return;
    }

    const int w = rc.x1 - rc.x0;

    for (int y = rc.y0; y < rc.y1; y += DISPLAY_BAND_LINES) {
        const int lines = ((rc.y1 - y) < DISPLAY_BAND_LINES) ? (rc.y1 - y) : DISPLAY_BAND_LINES;
        uint16_t *buf = display_band_buffer();

        /* 背景不是纯黑：外圈还有一圈装饰环。脏矩形的角可能伸到环上，
         * 所以按半径逐像素重建背景，而不是整块填黑（否则环会被擦出缺口）。 */
        for (int yy = 0; yy < lines; yy++) {
            const int dy = (y + yy) - UI_RING_CY;
            uint16_t *row = &buf[(size_t)yy * (size_t)w];
            for (int xx = 0; xx < w; xx++) {
                const int dx = (rc.x0 + xx) - UI_RING_CX;
                const int d2 = dx * dx + dy * dy;
                row[xx] = (d2 >= RING_IN2 && d2 <= RING_OUT2) ? RING_COLOR : COLOR_BLACK;
            }
        }

        /* 涟漪在最下层，被球压住是正常的 */
        for (int i = 0; i < RIPPLE_MAX; i++) {
            const ripple_t *rp = &s_ripple[i];
            if (rp->age < 0 || rp->age > RIPPLE_LIFE) {
                continue;
            }
            const int r = RIPPLE_R0 + rp->age * RIPPLE_STEP;
            if (rp->x + r < rc.x0 || rp->x - r >= rc.x1 ||
                rp->y + r < y || rp->y - r >= y + lines) {
                continue;
            }
            ripple_paint(buf, w, y, lines, rc.x0, rp);
        }

        for (int i = 0; i < BALL_COUNT; i++) {
            const ball_t *b = &s_ball[i];
            const int rr = b->r + 1;

            /* 剔除判定要按「球 + 拖尾」的整体包围盒来算。只看球当前位置的话，
             * 只有拖尾经过、球已经离开的那几条横带会被跳过，轨迹就断成虚线。 */
            int bx0 = b->ix, bx1 = b->ix;
            int by0 = b->iy, by1 = b->iy;
            for (int t = 0; t < TRAIL_LEN; t++) {
                if (b->tx[t] < bx0) bx0 = b->tx[t];
                if (b->tx[t] > bx1) bx1 = b->tx[t];
                if (b->ty[t] < by0) by0 = b->ty[t];
                if (b->ty[t] > by1) by1 = b->ty[t];
            }

            if (bx1 + rr < rc.x0 || bx0 - rr >= rc.x1 ||
                by1 + rr < y || by0 - rr >= y + lines) {
                continue;
            }

            /* 拖尾：由远到近画，近的压住远的 */
            for (int t = TRAIL_LEN - 1; t >= 0; t--) {
                ui_disc(buf, w, y, lines, rc.x0, b->tx[t], b->ty[t], b->r - 1, b->tc[t]);
            }

            ui_disc(buf, w, y, lines, rc.x0, b->ix, b->iy, b->r, b->color);

            const int hr = (b->r >= 9) ? (b->r / 3) : 2;
            const int hx = b->ix - b->r / 3;
            const int hy = b->iy - b->r / 3;
            ui_disc(buf, w, y, lines, rc.x0, hx, hy, hr, b->hi);
        }

        display_band_push(rc.x0, y, w, lines);
    }
}

static bool rect_hit(const rect_t *a, const rect_t *b)
{
    return !(a->x1 < b->x0 || b->x1 < a->x0 || a->y1 < b->y0 || b->y1 < a->y0);
}

/* 相交的脏矩形合并成一个，避免同一块区域重复推屏（球撞在一起时常见） */
static int rect_merge(rect_t *rc, int n)
{
    bool changed = true;

    while (changed && n > 1) {
        changed = false;
        for (int i = 0; i < n && !changed; i++) {
            for (int j = i + 1; j < n; j++) {
                if (!rect_hit(&rc[i], &rc[j])) {
                    continue;
                }
                if (rc[j].x0 < rc[i].x0) rc[i].x0 = rc[j].x0;
                if (rc[j].y0 < rc[i].y0) rc[i].y0 = rc[j].y0;
                if (rc[j].x1 > rc[i].x1) rc[i].x1 = rc[j].x1;
                if (rc[j].y1 > rc[i].y1) rc[i].y1 = rc[j].y1;
                rc[j] = rc[--n];
                changed = true;
                break;
            }
        }
    }
    return n;
}

/* 只重绘这一帧发生变化的区域 */
static void scene_flush(void)
{
    rect_t rc[BALL_COUNT + RIPPLE_MAX];
    int n = 0;

    for (int i = 0; i < BALL_COUNT; i++) {
        const ball_t *b = &s_ball[i];
        const int r = b->r + BALL_GHOST;

        int x0 = b->ix, x1 = b->ix;
        int y0 = b->iy, y1 = b->iy;

        if (b->ox < x0) x0 = b->ox;
        if (b->ox > x1) x1 = b->ox;
        if (b->oy < y0) y0 = b->oy;
        if (b->oy > y1) y1 = b->oy;

        for (int t = 0; t < TRAIL_LEN + 1; t++) {
            if (b->tx[t] < x0) x0 = b->tx[t];
            if (b->tx[t] > x1) x1 = b->tx[t];
            if (b->ty[t] < y0) y0 = b->ty[t];
            if (b->ty[t] > y1) y1 = b->ty[t];
        }

        rc[n++] = (rect_t){x0 - r, y0 - r, x1 + r + 1, y1 + r + 1};
    }

    for (int i = 0; i < RIPPLE_MAX; i++) {
        const ripple_t *rp = &s_ripple[i];
        if (rp->age < 0) {
            continue;
        }
        /* 用最大半径算包围盒：涟漪消失的那一帧也要把最后一圈擦干净 */
        const int age = (rp->age > RIPPLE_LIFE) ? RIPPLE_LIFE : rp->age;
        const int r = RIPPLE_R0 + age * RIPPLE_STEP + 2;
        rc[n++] = (rect_t){rp->x - r, rp->y - r, rp->x + r + 1, rp->y + r + 1};
    }

    n = rect_merge(rc, n);
    for (int i = 0; i < n; i++) {
        rect_draw(&rc[i]);
    }
}

/* ---------------- 物理 ---------------- */

static void ripple_spawn(int x, int y, uint16_t color)
{
    if (s_ripple_budget <= 0) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < RIPPLE_MAX; i++) {
        if (s_ripple[i].age < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int oldest = 0;
        for (int i = 1; i < RIPPLE_MAX; i++) {
            if (s_ripple[i].age > s_ripple[oldest].age) {
                oldest = i;
            }
        }
        slot = oldest;
    }

    s_ripple[slot] = (ripple_t){x, y, 0, color};
    s_ripple_budget--;
}

static void seed_balls(void)
{
    for (int i = 0; i < BALL_COUNT; i++) {
        ball_t *b = &s_ball[i];
        b->r = BALL_R_MIN + (int)(esp_random() % (uint32_t)(BALL_R_MAX - BALL_R_MIN + 1));
        b->color = k_colors[i];
        b->hi = shade(b->color, 55);
        b->tc[0] = shade(b->color, -30);
        b->tc[1] = shade(b->color, -55);
        b->tc[2] = shade(b->color, -72);

        int px = UI_RING_CX;
        int py = UI_RING_CY;
        ui_polar(UI_RING_CX, UI_RING_CY, (int)(esp_random() % 360),
                 (int)frand(20.0f, 90.0f), &px, &py);

        b->x = (float)px;
        b->y = (float)py;
        b->ix = px;
        b->iy = py;
        b->ox = px;
        b->oy = py;

        for (int t = 0; t < TRAIL_LEN + 1; t++) {
            b->tx[t] = px;
            b->ty[t] = py;
        }

        const float ang = frand(0.0f, 6.2831853f);
        const float sp = frand(90.0f, 190.0f);
        b->vx = cosf(ang) * sp;
        b->vy = sinf(ang) * sp;
    }
}

static void wall_bounce(ball_t *b)
{
    const float dx = b->x - (float)UI_RING_CX;
    const float dy = b->y - (float)UI_RING_CY;
    const float d = sqrtf(dx * dx + dy * dy);
    const float lim = ARENA_EDGE - (float)b->r;

    if (d <= lim || d < 0.001f) {
        return;
    }

    const float nx = dx / d;
    const float ny = dy / d;

    b->x = (float)UI_RING_CX + nx * lim;
    b->y = (float)UI_RING_CY + ny * lim;

    const float vn = b->vx * nx + b->vy * ny;
    if (vn > 0.0f) {
        b->vx -= (1.0f + WALL_BOUNCE) * vn * nx;
        b->vy -= (1.0f + WALL_BOUNCE) * vn * ny;
    }

    /* 兜底：速度低到快停住时补一脚 */
    const float sp = speed_of(b);
    if (sp < MIN_SPEED) {
        if (sp < 1.0f) {
            b->vx = -nx * MIN_SPEED;
            b->vy = -ny * MIN_SPEED;
        } else {
            b->vx *= MIN_SPEED / sp;
            b->vy *= MIN_SPEED / sp;
        }
    }

    /* 在圆里做镜面反射，轨迹是固定碰撞参数的「花瓣」，不打破的话球会长期
     * 困在某条环带上、下半屏越堆越多。每次撞墙随机偏转一点点，够散开又不飘。
     * 只旋转方向、不改变速度大小，能量仍然守恒。 */
    const float th = frand(-0.09f, 0.09f);
    const float ct = cosf(th);
    const float st = sinf(th);
    const float nvx = b->vx * ct - b->vy * st;
    const float nvy = b->vx * st + b->vy * ct;
    b->vx = nvx;
    b->vy = nvy;
}

static void ball_collide(ball_t *a, ball_t *b)
{
    const float dx = b->x - a->x;
    const float dy = b->y - a->y;
    const float min_d = (float)(a->r + b->r);
    const float d2 = dx * dx + dy * dy;

    if (d2 >= min_d * min_d || d2 < 0.0001f) {
        return;
    }

    const float d = sqrtf(d2);
    const float nx = dx / d;
    const float ny = dy / d;
    const float overlap = (min_d - d) * 0.5f;

    /* 先分开，避免两球黏在一起反复触发 */
    a->x -= nx * overlap;
    a->y -= ny * overlap;
    b->x += nx * overlap;
    b->y += ny * overlap;

    const float rvn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    if (rvn >= 0.0f) {
        return;   /* 正在分开，不用处理 */
    }

    const float imp = -(1.0f + BALL_BOUNCE) * rvn * 0.5f;
    a->vx -= imp * nx;
    a->vy -= imp * ny;
    b->vx += imp * nx;
    b->vy += imp * ny;

    /* 撞得够狠才冒涟漪，接触点在两球连线上的 a 球表面 */
    if (imp > RIPPLE_MIN_V) {
        ripple_spawn((int)lroundf(a->x + nx * (float)a->r),
                     (int)lroundf(a->y + ny * (float)a->r),
                     shade(b->color, 40));
    }
}

static void simulate(float dt)
{
    int steps = (int)(dt / SUBSTEP) + 1;
    if (steps > 4) {
        steps = 4;
    }
    const float h = dt / (float)steps;

    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < BALL_COUNT; i++) {
            ball_t *b = &s_ball[i];
            b->vx += s_gx * h;
            b->vy += s_gy * h;
            b->x += b->vx * h;
            b->y += b->vy * h;

            const float sp = speed_of(b);
            if (sp > MAX_SPEED) {
                b->vx *= MAX_SPEED / sp;
                b->vy *= MAX_SPEED / sp;
            }
            wall_bounce(b);
        }

        for (int i = 0; i < BALL_COUNT; i++) {
            for (int j = i + 1; j < BALL_COUNT; j++) {
                ball_collide(&s_ball[i], &s_ball[j]);
            }
        }

        /* 分离重叠时可能把球顶出边界，画之前再夹一次：保证球的脏矩形
         * （半径 + BALL_GHOST）永远够不到内径 166px 的外圈装饰环。 */
        for (int i = 0; i < BALL_COUNT; i++) {
            ball_t *b = &s_ball[i];
            const float dx = b->x - (float)UI_RING_CX;
            const float dy = b->y - (float)UI_RING_CY;
            const float d = sqrtf(dx * dx + dy * dy);
            const float lim = ARENA_EDGE - (float)b->r;
            if (d > lim && d > 0.001f) {
                b->x = (float)UI_RING_CX + dx / d * lim;
                b->y = (float)UI_RING_CY + dy / d * lim;
            }
        }
    }

    for (int i = 0; i < BALL_COUNT; i++) {
        ball_t *b = &s_ball[i];
        b->ix = (int)lroundf(b->x);
        b->iy = (int)lroundf(b->y);
    }
}

/* ---------------- 对外接口 ---------------- */

void anim_start(void)
{
    /* 和 snake_start() 一样走离屏合成，切页时不会先黑一下。
     * face_render() 已经 begin 过一次，帧锁可重入，这里不会提前推屏。 */
    display_frame_begin();

    display_clear(COLOR_BLACK);

    /* 静态外圈：球够不到，只画这一次 */
    display_draw_ring(UI_RING_CX, UI_RING_CY, UI_RING_R, UI_RING_W,
                      0, 0, RING_COLOR, RING_COLOR, COLOR_BLACK);

    seed_balls();

    for (int i = 0; i < RIPPLE_MAX; i++) {
        s_ripple[i].age = -1;
    }
    s_ripple_budget = RIPPLE_BUDGET;
    s_gx = 0.0f;
    s_gy = 0.0f;
    s_imu_skip = 0;
    s_started = true;
    s_last_us = esp_timer_get_time();
    s_next_us = 0;

    for (int i = 0; i < BALL_COUNT; i++) {
        const ball_t *b = &s_ball[i];
        const rect_t rc = {b->ix - b->r - BALL_GHOST, b->iy - b->r - BALL_GHOST,
                           b->ix + b->r + BALL_GHOST + 1, b->iy + b->r + BALL_GHOST + 1};
        rect_draw(&rc);
    }

    display_frame_end();
}

void anim_poll(void)
{
    if (!s_started || s_paused) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now < s_next_us) {
        return;
    }

    float dt = (float)(now - s_last_us) / 1000000.0f;
    if (dt < 0.001f) {
        dt = 0.001f;
    }
    if (dt > DT_MAX) {
        dt = DT_MAX;
    }
    s_last_us = now;
    s_next_us = now + FRAME_US;

    gravity_update();
    s_ripple_budget = RIPPLE_BUDGET;

    for (int i = 0; i < BALL_COUNT; i++) {
        ball_t *b = &s_ball[i];
        b->ox = b->ix;
        b->oy = b->iy;

        for (int t = TRAIL_LEN + 1; t-- > 1; ) {
            b->tx[t] = b->tx[t - 1];
            b->ty[t] = b->ty[t - 1];
        }
        b->tx[0] = b->ix;
        b->ty[0] = b->iy;
    }

    simulate(dt);
    scene_flush();

    /* 涟漪老化：画完这一帧再推进，RIPPLE_LIFE + 1 那一帧只擦不画 */
    for (int i = 0; i < RIPPLE_MAX; i++) {
        if (s_ripple[i].age < 0) {
            continue;
        }
        if (++s_ripple[i].age > RIPPLE_LIFE + 1) {
            s_ripple[i].age = -1;
        }
    }
}

void anim_set_paused(bool paused)
{
    if (paused == s_paused) {
        return;
    }

    s_paused = paused;
    if (!paused) {
        /* 恢复时把时间基准拉回现在，避免一帧补上几十秒的位移 */
        s_last_us = esp_timer_get_time();
        s_next_us = 0;
    }
}

void anim_kick(int x, int y)
{
    if (!s_started) {
        return;
    }

    for (int i = 0; i < BALL_COUNT; i++) {
        ball_t *b = &s_ball[i];
        const float dx = b->x - (float)x;
        const float dy = b->y - (float)y;
        const float d = sqrtf(dx * dx + dy * dy);

        if (d > 150.0f) {
            continue;
        }

        /* 越靠近触点力越大，方向从触点指向球 */
        const float k = 1.0f - d / 150.0f;
        if (d < 0.5f) {
            /* 正好点在球心：随机方向，别除以 0 */
            const float ang = frand(0.0f, 6.2831853f);
            b->vx += cosf(ang) * 300.0f * k;
            b->vy += sinf(ang) * 300.0f * k;
        } else {
            b->vx += dx / d * 300.0f * k;
            b->vy += dy / d * 300.0f * k;
        }
    }
}
