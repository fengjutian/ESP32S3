# hello_world — ESP32-S3 智能手表

基于 ESP-IDF 的智能手表固件，运行在微雪 **ESP32-S3-Touch-LCD-1.85B**（360×360 圆屏）上。
项目从 ESP-IDF 的 `hello_world` 示例改造而来：显示直接跑在 `esp_lcd` + ST77916 QSPI 上，
UI 是手写的行缓冲 / 脏区域渲染，没有引入 LVGL。

屏幕只有 360×360，表盘元素固定、刷新区域明确；手写渲染能把 RAM 和 Flash 压到最低，
也方便按脏区域做增量刷新。需要复杂界面或动画时再上 LVGL 也不迟。

## 功能

| 模块 | 能力 |
|---|---|
| 表盘 | 9 个：指针、简约、经典、极简、信息、天气、贪吃蛇、计步、语音 |
| 切换 | 轻触 / 左滑下一页，右滑上一页；第一页右滑不回绕 |
| 长按 | 切换 12/24 小时制；天气页长按立即刷新 |
| 语音 | 按住说话 → MiniMax ASR → 本地意图 / 大模型 → 流式 TTS 播报 |
| 计步 | QMI8658 峰值检测 + 步频校验，8000 步目标，按天存 NVS |
| 天气 | Open-Meteo 当前天气 / 湿度 / 风 / 高低温，默认 30 分钟刷新 |
| 时间 | 编译时间兜底 → SNTP 校准 → `esp_timer` 软件时钟 |
| 网络 | 网页配网，账号存 NVS；WiFi 引用计数，空闲 60 秒关闭 |
| 省电 | 30 秒无操作背光降到 15%，第一下触摸只唤醒 |
| 显示 | PSRAM 整页帧缓冲 + 脏区域刷新，中英混排 |

## 硬件

| 项 | 参数 |
|---|---|
| 开发板 | Waveshare ESP32-S3-Touch-LCD-1.85B |
| SoC | ESP32-S3R8，双核 LX7 @ 240MHz，512KB SRAM |
| Flash / PSRAM | 16MB / 8MB Octal PSRAM |
| 屏幕 | ST77916，QSPI，360×360，RGB565 |
| 触摸 | CST816S，I2C 地址 `0x15` |
| IMU | QMI8658，I2C `0x6B`（兼探 `0x6A`） |
| 音频 | ES8311 DAC `0x30` + ES7210 ADC `0x80` |
| 电量计 | BQ27220，I2C `0x55` |
| RTC | PCF85063，I2C `0x51` |

### 引脚

| 信号 | GPIO |
|---|---|
| LCD CS / PCLK / RST / BL | 21 / 40 / 3 / 5 |
| LCD D0 / D1 / D2 / D3 | 46 / 45 / 42 / 41 |
| I2C SCL / SDA | 10 / 11 |
| 触摸 RST / INT | 1 / 4 |
| RTC INT | 6 |
| I2S MCLK / BCLK / LRCK | 2 / 48 / 38 |
| I2S DIN / DOUT / PA | 39 / 47 / 9 |

共享 I2C 上挂了触摸、RTC、电量计、IMU 和两个音频 codec，共 6 个器件，
所以总线速率降到 **100kHz**（400kHz 会偶发位错误）。改速率见
[main/board_config.h](main/board_config.h) 的 `BOARD_I2C_HZ`。

> **占用提示**：GPIO19/20 是 USB，GPIO43/44 是串口控制台，GPIO0 是 BOOT，
> GPIO12~17 是 Micro SD（SDMMC 4-bit），GPIO2/38/39/47/48 是 I2S 音频，不要随意复用。

## 快速开始

### 环境

本机使用 EIM 安装的 ESP-IDF v6.1：

| 项 | 路径 / 版本 |
|---|---|
| ESP-IDF | v6.1，`C:\esp\v6.1\esp-idf` |
| 工具链 | `C:\Espressif\tools` |
| Python 虚拟环境 | `C:\Espressif\tools\python\v6.1\venv` |
| 目标芯片 | `esp32s3` |

`idf.py` 不在系统 PATH 里，**每个新终端都要先激活**：

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
```

### 配置

```powershell
cd C:\Users\Charles.feng\hello_world

# 首次配置时复制模板；已经有 app_config.h 就跳过，别覆盖自己的 Key
copy main\app_config.example.h main\app_config.h
```

然后按需修改 [main/app_config.h](main/app_config.h)：

| 宏 | 作用 |
|---|---|
| `APP_NTP_SERVER` | SNTP 服务器，默认 `ntp.aliyun.com` |
| `APP_TZ` | POSIX 时区，默认 `CST-8`（UTC+8） |
| `WEATHER_CITY` / `WEATHER_LAT` / `WEATHER_LON` | 天气页城市名和坐标 |
| `WEATHER_REFRESH_MIN` | 天气刷新周期，默认 30 分钟 |
| `APP_FORCE_PROVISION` | `1` = 每次开机强制配网，测试用 |
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | 仅作 NVS 为空时的兜底，通常留空 |
| `MINIMAX_API_KEY` | 留空 = 语音问答不可用，其余功能正常 |
| `MINIMAX_BASE_URL` | 国内 `https://api.minimax.cn`，海外 `https://api.minimax.io` |
| `MINIMAX_TTS_VOICE` | TTS 音色 ID |

模型、录音时长、麦克风增益、音量、系统提示词等参数在
[main/ai_config.h](main/ai_config.h) 里，可以在 `app_config.h` 中用同样的宏覆盖。

> **密钥提醒**：`main/app_config.h` 可能包含 WiFi 密码和 MiniMax API Key。
> 它虽然在 `.gitignore` 里，但如果曾经用 `git add -f` 加过，Git 仍会继续跟踪。
> 公开仓库前用 `git ls-files main/app_config.h` 检查；有输出就执行
> `git rm --cached main/app_config.h`，并轮换已经泄露的 Key。

### 编译烧录

```powershell
# 0. 激活环境（新终端必做）
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"

cd C:\Users\Charles.feng\hello_world

# 1. 设定目标：首次构建或换芯片时执行，会重置 sdkconfig
idf.py set-target esp32s3

# 2. 编译
idf.py build

# 3. 烧录并监视串口（Ctrl+] 退出）
idf.py -p COMx flash monitor
```

`COMx` 换成设备管理器里的实际端口（例如 `COM3` / `COM5`）。监视时按
`Ctrl+T` 再按 `Ctrl+R` 可以复位芯片，方便看完整启动日志。

改过 PSRAM、Flash 容量或分区表后，必须先 `idf.py fullclean` 再编译，
否则链接脚本和配置还是旧的。

### 首次开机与配网

账号不需要写死在代码里。首次开机（NVS 里没有账号、也没有配置兜底账号）会自动进入配网模式：

1. 屏幕提示「WiFi 配网」，手机连接热点 **`ESP-Watch`**（密码 **`12345678`**）。
2. 浏览器打开 **`http://192.168.4.1`**。
3. 填写家里的 WiFi 名称和密码，点「保存并连接」。
4. 设备自动重启并连接，账号存进 NVS，以后开机直接使用。

配网热点只开 3 分钟，超时自动关闭，手表离线走时。连续 3 次连接失败会自动重新开热点。
热点名和密码写在 [main/wifi_prov.c](main/wifi_prov.c)；3 分钟超时和失败次数在
[main/wifi_time.c](main/wifi_time.c) 的 `PROV_TIMEOUT_MS` / `MAX_FAIL_STREAK` 里。

想强制重新配网，把 `main/app_config.h` 里的 `APP_FORCE_PROVISION` 改成 `1`
烧录一次，测完改回 `0`。

## 使用

### 手势

普通手势的判定都在 [main/watch.c](main/watch.c) 的 `gesture_feed()` 里，带鬼触过滤：
按压至少 3 帧（约 60ms）才算数，单帧坐标跳变超过 120px 丢弃，手势之间冷却 500ms。
AI 页的「按住说话」在 `watch_task()` 里单独处理，不经过这套滑动判定。

**普通表盘**

- **轻触**（按住 < 0.8 秒）：切到下一个表盘。
- **左滑**（水平位移 ≥ 60px）：切到下一个表盘。
- **右滑**（水平位移 ≥ 60px）：切到上一个表盘；第一页（指针）右滑不回绕，
  避免一划手就进语音页。
- **长按**（按住 > 1 秒）：切换 12/24 小时制；天气页上则是立即刷新天气。
- 按住 0.8~1 秒是死区，既不算点按也不算长按，直接忽略，防止误触。

**贪吃蛇**

- 点四个方向键，或点棋盘外侧任意位置：转向。
- 点棋盘内部：暂停 / 继续。
- 游戏结束后点任意位置：重开一局。
- 右上角「退出」：离开游戏。
- 任意方向滑动或长按：切到下一个表盘。

**语音页**

- **按住**（连续 2 帧，约 40ms）：开始录音，松手发送。
- 按住不到 0.5 秒：当误触取消；如果手里有上一轮回答，就重播一遍。
- 处理中再按一下：打断当前一轮（播放会立刻停，正在进行的请求返回后结束）。
- 左右滑动：取消当前请求并切走。

**通用**

- 30 秒无操作，背光降到 15%；熄屏后的第一下触摸只负责点亮屏幕，
  不会顺带切页。
- 每次有效手势后有 500ms 冷却，防止一次抖动连切好几页。

### 表盘

切换顺序就是下表顺序：

| # | 表盘 | 内容 | 刷新方式 |
|---|---|---|---|
| 1 | 指针 | 秒环、60 格刻度、渐细时分秒针、日期 + 电量窗 | 每秒只重绘指针扫过的区域 |
| 2 | 简约 | 无刻度无秒环，三根指针 + 日期 + 电量 | 每秒脏区域 |
| 3 | 经典 | 秒环 + 大字号时间 + 中文日期 + 星期 + 电量 | 秒环增量 + 分钟变化重画文字 |
| 4 | 极简 | 超大时间 + 秒数 + 日期 + 电量 | 秒数每秒，大字按分钟 |
| 5 | 信息 | 电量、电压、时间、日期、运行时长、网络状态 | 每秒刷新行 |
| 6 | 天气 | 城市 + 时间、天气图标、温度、描述、高低温、湿度、风、日期 | 分钟改时间，数据到达整页重画 |
| 7 | 贪吃蛇 | 18×18 棋盘，吃食物加速，最高分存 NVS | 20ms 轮询、按节拍推进 |
| 8 | 计步 | 大号步数、目标百分比、估算里程、进度环 | 步数变化时增量更新 |
| 9 | 语音 | AI 状态、问题、回答，等待时显示已等秒数 | 内容变化时局部重画 |

天气图标是根据 WMO 天气码和 `is_day` 程序化画出来的（晴 / 月 / 云 / 雨 / 雪），
没有图片资源。计步里程按 0.7m 一步估算。

### 语音助手

链路全部在设备端完成，不需要自建服务器：

```
按住说话 → 16kHz 单声道 WAV → MiniMax ASR → 本地意图？
                                              ├─ 是 → 直接拼答案
                                              └─ 否 → 大模型 → 流式 TTS → ES8311 → 喇叭
```

- 录音 16kHz / 16bit / 单声道，上限 8 秒，最短 0.25 秒；缓冲区放 PSRAM。
- ASR 用 `asr-1.0`，大模型默认 `MiniMax-M3.1-Flash`，TTS 默认 `speech-2.8-turbo`。
- 回答要求 40 字以内，`reasoning_effort=low`，`max_completion_tokens=160`，尽量快。
- TTS 边收边播，第一段音频到了就开始出声，不用等整段合成完。

下面这些问法**不走大模型**，识别完直接本地回答（省一次请求、省 token、快 2~3 秒）：

| 意图 | 触发词示例 | 数据来源 |
|---|---|---|
| 时间 | 几点、时间、时刻 | 表盘软件时钟（SNTP 校准） |
| 日期 | 几号、日期、星期、礼拜、几月 | 同上 |
| 天气 | 天气、气温、温度、下雨、冷不冷、热不热 | Open-Meteo 缓存 |
| 电量 | 电量、电池、多少电、还有电、几格电 | BQ27220 |
| 步数 | 步数、走了多少、几步 | 计步器 |
| 身份 / 客套 | 你是谁、你叫什么、谢谢、再见 | 固定话术 |

匹配不上（比如「讲个笑话」）才走云端。实现见 [main/local_intent.c](main/local_intent.c)。

API Key 和网关区域绑定：国内账号用 `https://api.minimax.cn`，海外账号用
`https://api.minimax.io`，填错会返回 401 / 404。没有填 Key 时语音页会提示
「未配置 API Key」，其他功能不受影响。

完整的接口参数、时序、内存布局和排错表见
**[docs/AI_VOICE_MINIMAX.md](docs/AI_VOICE_MINIMAX.md)**。

### 计步

[main/pedometer.c](main/pedometer.c) 用 QMI8658 以 50Hz 采样加速度模值：

1. 一阶低通得到基线（重力 + 缓慢姿态变化）。
2. `|a| - 基线` 超过 0.12g 记一次峰值，回落到 0.02g 记一次步子事件。
3. 最近几次步子的间隔要满足「均值 340~1000ms、变异系数 < 25%」才确认是走路，
   摇表、颠簸这类随机运动不会加分。
4. 超过 2 秒没有新步子，未确认的步子作废。
5. 步数按天存 NVS，跨天自动清零；目标固定 8000 步。

算法参数都在文件顶部的宏里，换佩戴方式或觉得太灵敏 / 太迟钝时可以调。

## 后台行为

| 功能 | 行为 |
|---|---|
| 时间 | 开机用 `__DATE__` / `__TIME__` 播种；SNTP 成功后校准软件时钟；之后由 `esp_timer` 累加 |
| RTC | PCF85063 只做 best-effort 写入，**不读它作为时间来源** |
| 天气 | Open-Meteo，默认 30 分钟一次；天气页长按可立即刷新；数据到达后重画天气盘 |
| WiFi | `net_link` 引用计数，最后一次 `release()` 后保持 60 秒再关，连续语音请求不用反复重连 |
| 电量 | BQ27220 读电压，每 5 秒刷新；电量百分比由电压曲线插值估算 |
| 省电 | 30 秒无操作背光降到 15%，触摸立即恢复 |
| 看门狗 | 表盘主循环注册 TWDT，5 秒不喂狗直接 panic 重启，避免冻在最后一帧 |

## 代码结构

```
hello_world/
├── CMakeLists.txt                 顶层工程定义（MINIMAL_BUILD）
├── sdkconfig.defaults             可提交的默认配置（16MB flash / 自定义分区 / Octal PSRAM）
├── partitions.csv                 分区表：app 分区 4MB
├── pytest_hello_world.py          原示例的 pytest 脚本，和手表功能无关
├── docs/
│   └── AI_VOICE_MINIMAX.md        语音问答接入、时序与排错
├── tools/
│   └── minimax_check.py           PC 端 MiniMax 自检脚本
└── main/
    ├── CMakeLists.txt             组件依赖清单
    ├── idf_component.yml          组件管理器依赖
    ├── app_config.example.h       配置模板
    ├── app_config.h               本地配置（含密钥，不要提交）
    ├── ai_config.h                AI 模型 / 音频参数
    ├── board_config.h             板级硬件配置（屏幕 / 触摸 / I2C / I2S）
    ├── main.c                     应用入口：初始化外设并启动任务
    ├── display.c / display.h      ST77916 驱动：填充、文本、圆环、帧缓冲
    ├── st77916_init_cmds.h        厂商 v1 / v2 初始化序列（生成）
    ├── font8x16.h                 8×16 ASCII 字模（生成）
    ├── font16x16_zh.h             16×16 GB2312 中文字模（生成）
    ├── ui.c / ui.h                UI 共享层：表盘枚举、状态、文本与矢量绘制
    ├── faces.c / faces.h          9 个表盘的绘制与分发
    ├── watch.c / watch.h          表盘主任务：软时钟、手势、切换、背光
    ├── touch.c / touch.h          CST816S 触摸驱动
    ├── board_i2c.c / board_i2c.h  共享 I2C 总线
    ├── rtc.c / rtc.h              PCF85063 驱动（仅备份写入）
    ├── bq27220.c / bq27220.h      BQ27220 电量计（电压估算 SOC）
    ├── imu.c / imu.h              QMI8658 加速度计
    ├── pedometer.c / pedometer.h  计步算法（峰值 + 步频校验）
    ├── weather.c / weather.h      Open-Meteo 拉取与极简 JSON 解析
    ├── wifi_prov.c / wifi_prov.h  SoftAP + 网页配网，账号存 NVS
    ├── net_link.c / net_link.h    WiFi 引用计数与保活
    ├── wifi_time.c / wifi_time.h  SNTP 校时 + 天气刷新后台任务
    ├── minimax.c / minimax.h      ASR / Chat / TTS 三个云端接口
    ├── audio_io.c / audio_io.h    I2S 全双工 + ES8311 / ES7210
    ├── local_intent.c / local_intent.h  本地意图匹配
    ├── voice_ai.c / voice_ai.h    语音问答状态机与 AI 表盘
    └── snake.c / snake.h          贪吃蛇逻辑与增量渲染
```

依赖组件：`espressif/esp_lcd_st77916` ^2.0.2、`espressif/esp_lcd_touch`、
`espressif/esp_lcd_touch_cst816s`、`espressif/esp_codec_dev`。

### 任务划分

| 任务 | 栈 | 优先级 | 职责 |
|---|---|---|---|
| `watch` | 6KB | 5 | 20ms 主循环：手势、切页、软时钟、背光、电量、计步 |
| `voice_ai` | 20KB | 5 | 录音 → ASR → 对话 → TTS → 播放，整条链路串行 |
| `net` | 6KB | 4 | SNTP 校时、天气刷新、触发配网 |
| `prov` | 4KB | 4 | SoftAP + HTTP 配网服务 |
| `net_idle` | 3KB | 2 | 引用归零 60 秒后关闭 WiFi |
| `app_main` | - | - | 初始化 NVS / 网络 / 屏幕 / I2C / 传感器，启动以上任务 |

表盘任务不直接发网络请求，也不操作 I2S；语音任务只更新共享文字（互斥锁保护），
`voice_ai_render()` 由表盘任务调用。这样不会出现两个任务同时操作
I2S 或显示器的竞态。

### 渲染管线

1. `face_render()` 先调 `display_frame_begin()`，把整页画进 PSRAM 帧缓冲
   （360×360×2 ≈ 253KB），最后 `display_frame_end()` 一次性推屏。
   切页时不会出现「先清成黑屏、再逐块画出来」的闪烁。
2. 运行中的 `face_tick()` 只重绘脏区域：指针只擦指针，经典表盘文字只在
   分钟变化时重画，计步只更新数字和新增的进度环，天气每分钟只改顶部时间行。
3. 推屏按 16 行一批走 DMA（`LINE_CHUNK`），避免单次传输超过 `max_transfer_sz`。
4. `push_bitmap()` 统一做一次字节交换，所有绘制路径（填充、文字、圆环、
   行缓冲）都会生效。
5. `display_draw_ring()` 会重绘整个圆形包围盒，`display_draw_arc()` 只重绘
   扇形包围盒；前者之后必须补画圆内文字。

显示 API 的最小用法：

```c
#include "display.h"

display_init();
display_clear(COLOR_BLACK);
display_draw_text_utf8(40, 80, "你好", 2, COLOR_WHITE, COLOR_BLACK);

int w = display_text_width_utf8("你好", 2);   // 计算宽度，便于居中
```

坐标系原点在左上角，x 向右、y 向下，超出屏幕的内容会自动裁剪。

### 显示微调

改 [main/board_config.h](main/board_config.h)，改完重新编译烧录：

| 现象 | 改法 |
|---|---|
| 颜色整体反了（黑变白） | `LCD_INVERT_COLOR` 0 → 1 |
| 红蓝互换 | [main/display.c](main/display.c) 里 `LCD_RGB_ELEMENT_ORDER_RGB` → `BGR` |
| 花屏 / 撕裂 / 斜纹 | `LCD_PIXEL_CLOCK_HZ` 80MHz → 40MHz → 20MHz |
| 画面上下 / 左右颠倒 | `LCD_MIRROR_X` / `LCD_MIRROR_Y` 改 1 |
| 想横屏显示 | `LCD_SWAP_XY` 改 1，并把 `LCD_H_RES` / `LCD_V_RES` 对调 |
| 触摸方向和屏幕不一致 | `TOUCH_SWAP_XY` / `TOUCH_MIRROR_X` / `TOUCH_MIRROR_Y` |

### 新增一个表盘

1. 在 [main/ui.h](main/ui.h) 的 `face_t` 里加枚举值（放在 `FACE_COUNT` 之前），
   并在 [main/ui.c](main/ui.c) 的 `ui_face_name` 里补名字。
2. 在 [main/faces.c](main/faces.c) 里实现 `xxx_render()` 和 `xxx_tick()`，
   接到 `face_render()` / `face_tick()` 的 `switch` 里。
3. 如果新表盘需要特殊手势，在 [main/watch.c](main/watch.c) 里加分支；
   普通「轻触 / 左右滑」不需要改。
4. 只有新增 `.c` 文件时才需要改 [main/CMakeLists.txt](main/CMakeLists.txt)。

## 踩坑记录

1. **这块屏是 ST77916 QSPI，不是 ST7789 SPI。**
   单线 SPI 的驱动和引脚定义全部不适用，必须用 4 条数据线的 QSPI。

2. **`espressif/esp_lcd_st77916` 自带的默认初始化序列不适用。**
   它的默认序列是给另一款 ST77916 面板的，烧上去就是雪花屏。
   本工程用微雪 BSP 的 v1 / v2 序列，并先以 3MHz 读寄存器 `0x04` 自动选择：

   | 寄存器 `0x04` 返回值 | 版本 |
   |---|---|
   | `00 7F 7F 7F` | v1 |
   | `00 02 7F 7F` | v2 |
   | 读取失败 | 回落 v1 |

   本板实测为 **`00 02 7F 7F` → v2**。微雪 Arduino 示例里那套是 v1，
   用在这块板上只会出雪花。

3. **像素字节序要在软件里交换一次。**
   厂商初始化序列里有 4 条 `0xC8`（RAMCTL），它固定了面板字节序，会覆盖
   `esp_lcd_panel_dev_config_t.data_endian`，设成 `LITTLE` 也不起作用。
   现象：青色 → 黄色、灰色 → 深蓝、绿色 → 洋红（白色对称，看不出来）。
   现在统一在 `push_bitmap()` 里交换，见 [main/display.c](main/display.c)。

4. **`display_draw_ring()` 会重绘整个圆形包围盒。**
   非环区域填背景色，所以圆环之后必须重画所有文字，否则日期 / 星期会被
   每秒一次的圆环刷新擦掉。只要画一小段弧时用 `display_draw_arc()`，
   它的包围盒不会把圆心和文字一起扫掉。

5. **屏的可视区是圆形。**
   面板是 360×360，但只有内切圆可见，四角被外壳遮住。排版要收在半径
   约 165 的圆内，否则日期 / 状态行会落到看不见的区域；秒环半径 172 在最外圈。

6. **这块板上的 RTC 不能作为时间来源。**
   PCF85063（`0x51`）实测年份寄存器 `0x08` 写不进去，偶发位错误
   （小时读到 `0x33`，或 00 读成 04），降速到 100kHz 依旧。
   因此时间以 SNTP + 软件时钟（`esp_timer`）为准，RTC 只做 best-effort 写入，
   代码里也不读它。代价是断电后从编译时间重来，开机后自动校准。

7. **BQ27220 的 StateOfCharge（`0x2C`）不可信。**
   未配置电池建模参数时，4.1V 会报 12%。电压寄存器（`0x08`）是准的，
   所以电量改为按电压曲线插值估算（4.2V=100%，3.3V=0%，见
   [main/bq27220.c](main/bq27220.c) 的 `SOC_CURVE`）。

8. **PSRAM 是 Octal，`sdkconfig` 又被 `.gitignore` 忽略。**
   Flash 容量、分区表、PSRAM 模式、TWDT panic 这些必须写在
   [sdkconfig.defaults](sdkconfig.defaults) 里，否则 `set-target` 后会退回默认值。
   本板是 16MB flash + 8MB Octal PSRAM；改完 PSRAM 配置记得 `idf.py fullclean`。

9. **触摸有鬼触，熄屏唤醒也会误触。**
   除了按压帧数 / 位移过滤，熄屏后的第一下只负责点亮背光，不参与手势；
   否则每唤醒一次就切一页，最后常常停在语音页。

10. **0.8~1 秒的按住是死区。**
    < 0.8 秒算点按，> 1 秒算长按，中间直接忽略。这是为了区分「想点一下」
    和「想长按」，避免长按刚触发点按就切页。

11. **WiFi 不是用完立即关。**
    语音一轮要连发 ASR、Chat、TTS 三个请求，反复重连会多花 2~3 秒。
    [main/net_link.c](main/net_link.c) 用引用计数管理 WiFi，最后一次 release
    后保持 60 秒；天气刷新仍然是 30 分钟一次。

12. **API Key 会被编译进固件。**
    能从 flash 里读出来。个人玩具没问题，但不要把带 key 的固件发到网上；
    如果要量产或公开，加一层自己的代理服务，设备只带短期 token。

13. **UI 卡死会被看门狗重启。**
    表盘任务注册了任务看门狗，主循环 5 秒不喂狗就直接 panic 重启，
    不会把最后一帧冻在屏幕上、点什么都没反应。

## 路线图

- **抬腕亮屏 / 摇一摇换表盘**：IMU 已经在跑，补手势识别即可。
- **上 LVGL**：需要复杂界面、动画或列表时再引入 `lvgl/lvgl` + `esp_lvgl_port`。
- **更多小游戏**：2048 / 俄罗斯方块，复用贪吃蛇的增量渲染框架。
- **音乐播放**：SD 卡 MP3 → ES8311 输出。
- **OTA**：现在只能插线烧录，加分区和 HTTPS OTA 后可以远程升级。
- **本地 VAD / 唤醒词**：不用一直按住说话。

## 参考

- [微雪 ESP32-S3-Touch-LCD-1.85B 文档](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B)
- [官方示例仓库](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B)
  （`Examples/ESP-IDF-V5.5.3/` 下是 BSP 与各示例）
- [espressif/esp_lcd_st77916 组件](https://components.espressif.com/components/espressif/esp_lcd_st77916)
- [ESP-IDF v6.1 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/index.html)
- [Open-Meteo 文档](https://open-meteo.com/en/docs)
- [MiniMax 开放平台文档](https://platform.minimax.io/docs)

## 许可

本工程源自 ESP-IDF 的 `hello_world` 示例（CC0-1.0）。
ST77916 初始化序列来自微雪 BSP（Apache-2.0）。
