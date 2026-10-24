# hello_world — ESP32-S3-Touch-LCD-1.85B

基于 ESP-IDF 的 `hello_world` 示例改造而成的 **智能表盘**，运行在
微雪 (Waveshare) **ESP32-S3-Touch-LCD-1.85B** 开发板上（360×360 触摸屏）。

## 功能

- **五种表盘**，轻触循环切换（也可左右滑动）
  - 指针：外圈秒针圆环 + 60 格刻度 + 渐细时分秒针 + 日期窗
  - 简约：无刻度无秒环，只有三根指针 + 日期窗
  - 经典：秒环 + 大字号时间 + 中文日期 + 星期 + 电池
  - 极简：超大时间 + 秒数
  - 信息：电量 / 电压 / 时间 / 日期 / 运行时长
  - 天气：当前温度 / 天气描述 / 湿度 / 时间 / 日期
  - 贪吃蛇：12×12 棋盘小游戏（滑动转向 / 轻触重开 / 长按退出）
  - 语音：AI 语音问答（按住说话 → MiniMax 识别 → 大模型 → 语音播报）
- **AI 语音问答**：切到「语音」表盘，按住屏幕说话，松手后走
  MiniMax `asr-1.0` → `MiniMax-M3.1-Flash` → `speech-2.8-turbo`，
  回答显示在屏幕上并由喇叭读出；轻点重播，滑动切走。
  接入步骤、参数和排错见 **[docs/AI_VOICE_MINIMAX.md](docs/AI_VOICE_MINIMAX.md)**
- **交互**：轻触切换表盘；长按 1 秒切换 12/24 小时制（**天气盘上长按 = 立即刷新**）
- **省电**：30 秒无操作背光降到 15%，触摸立即恢复（LEDC PWM 调光）
- **天气**：Open-Meteo 免费 API（不需要 key），含天气图标、今日高低温、湿度、风力；
  开机拉取、每 30 分钟刷新，拉完即关 WiFi
- **中文显示**：GB2312 一级汉字 + 符号，16×16 点阵，中英混排
- **RTC 走时**：PCF85063，掉电自动用编译时间重新播种
- **联网校时**：开机后台连 WiFi + SNTP，校准后写回 RTC 并关闭 WiFi
- **电量显示**：BQ27220 电量计，每 5 秒刷新
- **刷新策略**：切页 / 重开游戏先把整页画进 PSRAM 帧缓冲（253 KB）再一次性推屏，
  不再「先清黑再逐块画」；运行中按脏区域增量刷新——指针只擦指针、经典表盘文字
  只在分钟变化时重画、步数只更新数字和新增进度环、天气每分钟只改顶部时间行

```
     经典表盘                              信息表盘
   ╭─────────╮                        ┌─────────────┐
   │  23:29  │  ← 白色 4 倍字          │   设备信息   │
   │ 2026年  │  ← 青色 2 倍字          │ 电量   100%  │
   │ 10月23日 │                        │ 电压   4.12V │
   │ 星期五   │                        │ 时间   23:29 │
   │ 电池 100%│                        │ 运行   00:03 │
   ╰─────────╯                        └─────────────┘
```

---

## 1. 硬件

| 项 | 参数 |
|---|---|
| 开发板 | Waveshare ESP32-S3-Touch-LCD-1.85B（SKU 34554） |
| SoC | ESP32-S3R8，双核 LX7 @240MHz，512KB SRAM |
| Flash / PSRAM | 16MB / 8MB（叠封） |
| LCD | **ST77916**，**QSPI** 接口，**360×360**，RGB565（262K 色） |
| 触摸 | CST816S（I2C 地址 `0x15`） |
| IMU | QMI8658（I2C `0x6B`） |
| 音频 | ES8311（`0x30`）+ ES7210（`0x80`） |
| 电量计 | BQ27220（I2C `0x55`） |
| RTC | PCF85063（I2C `0x51`） |

### LCD 引脚（代码里已按此配置）

| 信号 | GPIO | 信号 | GPIO |
|---|---|---|---|
| LCD_CS | 21 | LCD_DATA0 | 46 |
| LCD_PCLK | 40 | LCD_DATA1 | 45 |
| LCD_RST | 3 | LCD_DATA2 | 42 |
| LCD_BL（背光） | 5 | LCD_DATA3 | 41 |

共享 I2C：SCL=GPIO10，SDA=GPIO11。

> **占用提示**：GPIO19/20 是 USB，GPIO43/44 是串口控制台，GPIO0 是 BOOT，
> GPIO12~17 是 Micro SD（SDMMC 4-bit），GPIO2/38/39/47/48 是 I2S 音频，别乱用。

---

## 2. 开发环境

本机已安装（EIM 安装器）：

| 项 | 路径 / 版本 |
|---|---|
| ESP-IDF | v6.1，`C:\esp\v6.1\esp-idf` |
| 工具链 | `C:\Espressif\tools` |
| Python 虚拟环境 | `C:\Espressif\tools\python\v6.1\venv` |
| 目标芯片 | `esp32s3` |

`idf.py` 不在系统 PATH 中，**每个新开的终端都要先激活**：

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
```

---

## 3. 构建 / 烧录 / 运行

```powershell
# 0. 激活环境（新终端必做）
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"

cd C:\Users\Charles.feng\hello_world

# 1. 设定目标（换芯片或首次构建才需要，会重置 sdkconfig）
idf.py set-target esp32s3

# 2. 编译
idf.py build

# 3. 烧录 + 串口监视（Ctrl+] 退出）
idf.py -p COM3 flash monitor

# 只烧录不监视
idf.py -p COM3 flash

# 监视时想看完整启动日志，按 Ctrl+T 然后 Ctrl+R 复位芯片
```

### 烧录成功时的串口日志

```
I (761) pcf85063: PCF85063 ready at 0x51 (仅作备份，时间以 SNTP 为准)
I (761) bq27220: BQ27220 ready at 0x55, 4122 mV, 100%
I (821) touch: CST816S ready at 0x15
I (1511) watch: now 2026-10-24 00:16:00
I (4991) watch: clock set to 2026-10-24 00:16:24      ← SNTP 校准后
```

### 时间来源

1. 开机用固件的编译时间（`__DATE__` / `__TIME__`）兜底
2. 约 5 秒后 WiFi + SNTP 校准，软件时钟对准到本地时间（`APP_TZ`）
3. 之后由 `esp_timer` 累加计时

RTC 仍会写入，但这块板上的 PCF85063 年份寄存器异常，**不读它**（详见第 5 节第 11 条）。

### 启用联网校时（WiFi 配网）

**不需要改代码**。首次开机会自动进入配网模式：

1. 屏幕提示「WiFi 配网」，手机连热点 **`ESP-Watch`**（密码 **`12345678`**）
2. 浏览器打开 **`http://192.168.4.1`**
3. 填写家里的 WiFi 名称和密码，点「保存并连接」
4. 设备自动重启并连接，账号存进 NVS，**以后开机直接用，不再询问**

配网热点只开 3 分钟；没配完则离线走时（本地时钟照常运行）。
连续 3 次连接失败会自动重新开热点配网。

想强制重新配网，把 [main/app_config.h](main/app_config.h) 里的
`APP_FORCE_PROVISION` 改成 `1` 烧录一次即可（测完改回 0）。

`APP_NTP_SERVER`、`APP_TZ` 等仍在该文件里配置。

校时成功后日志会打印 `time synced: 2026-10-24 11:58:12`，随后自动关闭 WiFi
（省电、降发热）；天气每 30 分钟联网 6 秒刷新一次。

---

## 4. 屏幕相关代码

| 文件 | 作用 |
|---|---|
| [main/board_config.h](main/board_config.h) | **硬件配置**：分辨率、QSPI 引脚、时钟、初始化版本、镜像/反色 |
| [main/display.c](main/display.c) | ST77916 初始化 + 填充 + 文本绘制（DMA 分行刷屏） |
| [main/display.h](main/display.h) | API 与 RGB565 颜色宏 |
| [main/st77916_init_cmds.h](main/st77916_init_cmds.h) | 厂商 v1（182 条）/ v2（185 条）初始化序列 |
| [main/font8x16.h](main/font8x16.h) | 8×16 ASCII 点阵字模（字符 32~126，Consolas 14px 生成） |
| [main/main.c](main/main.c) | `app_main`：初始化各外设并启动表盘、联网任务 |
| [main/ui.c](main/ui.c) / [ui.h](main/ui.h) | UI 共享层：表盘枚举、共享状态、矢量绘制原语 |
| [main/faces.c](main/faces.c) / [faces.h](main/faces.h) | 七个表盘的绘制实现与分发 |
| [main/snake.c](main/snake.c) / [snake.h](main/snake.h) | 贪吃蛇游戏逻辑与增量渲染 |
| [main/watch.c](main/watch.c) / [watch.h](main/watch.h) | 表盘主任务：软件时钟、手势、切换、背光省电 |
| [main/touch.c](main/touch.c) / [touch.h](main/touch.h) | CST816S 触摸驱动（轮询读坐标） |
| [main/rtc.c](main/rtc.c) / [rtc.h](main/rtc.h) | PCF85063 RTC 驱动（BCD 读写、掉电标志、星期计算） |
| [main/bq27220.c](main/bq27220.c) / [bq27220.h](main/bq27220.h) | BQ27220 电量计（电压、电量百分比） |
| [main/wifi_time.c](main/wifi_time.c) / [wifi_time.h](main/wifi_time.h) | 后台 WiFi + SNTP 校时，写入 RTC |
| [main/board_i2c.c](main/board_i2c.c) / [board_i2c.h](main/board_i2c.h) | 共享 I2C 总线（触摸/RTC/电量计） |
| [main/app_config.h](main/app_config.h) | NTP 服务器、时区、天气坐标、配网开关 |

### API 用法

```c
#include "display.h"
#include "board_config.h"

display_init();                                   // 失败会返回错误码，不影响串口部分

display_clear(COLOR_BLACK);                       // 整屏填充
display_fill_rect(0, 260, 360, 20, COLOR_BLACK);  // 局部填充（会自动裁剪）

display_draw_text(48, 96, "Hello World", 3,       // x, y, 文本, 放大倍数
                  COLOR_WHITE, COLOR_BLACK);      // 前景色, 背景色

int w = display_text_width("Hello World", 3);     // 计算宽度，便于居中
```

坐标系原点在左上角，x 向右、y 向下。文本超出屏幕会自动裁剪。

### 显示效果微调

改 [main/board_config.h](main/board_config.h)，改完重新编译烧录即可：

| 现象 | 改法 |
|---|---|
| 颜色整体反了（黑变白） | `LCD_INVERT_COLOR` 0 → 1 |
| 红蓝互换 | `display.c` 里 `LCD_RGB_ELEMENT_ORDER_RGB` → `BGR` |
| 花屏 / 撕裂 / 斜纹 | `LCD_PIXEL_CLOCK_HZ` 80MHz → 40MHz → 20MHz |
| 画面上下 / 左右颠倒 | `LCD_MIRROR_X` / `LCD_MIRROR_Y` 改 1 |
| 想横屏显示 | `LCD_SWAP_XY` 改 1，并把 `LCD_H_RES` / `LCD_V_RES` 对调 |

---

## 5. 关键结论（踩坑记录）

1. **这块屏不是 ST7789 SPI，而是 ST77916 QSPI。**
   单线 SPI 的驱动和引脚定义全部不适用，必须用 4 条数据线的 QSPI。

2. **`espressif/esp_lcd_st77916` 组件自带的默认初始化序列不适用。**
   它的默认序列是给另一款 ST77916 面板的，烧上去就是雪花屏。

3. **这块板有两个屏幕版本，初始化序列不同。**
   官方 BSP 的做法是：先用 3MHz 读寄存器 `0x04`，再据此选择序列。

   | 寄存器 `0x04` 返回值 | 版本 | 命令条数 |
   |---|---|---|
   | `00 7F 7F 7F` | v1 | 182 |
   | `00 02 7F 7F` | v2 | 185 |
   | 读取失败 | 回落到 v1 | 182 |

   本板实测为 **`00 02 7F 7F` → v2**。
   （微雪 Arduino 示例里抄到的那套是 v1，用它在这块板上只会出雪花。）

4. **像素时钟用 80MHz。** 官方 BSP 读版本时用 3MHz，读完切 80MHz。
   单次 DMA 传输不能超过 `max_transfer_sz`，所以绘制按 16 行分批（见 `LINE_CHUNK`）。

5. **`sdkconfig` 被 `.gitignore` 忽略**，所以 flash 容量这类设置写在
   [sdkconfig.defaults](sdkconfig.defaults) 里（本板 16MB），否则 `set-target`
   后会退回默认 2MB 并产生告警。

6. **VS Code 扩展的目标芯片** 在 [.vscode/settings.json](.vscode/settings.json)
   的 `idf.customExtraVars.IDF_TARGET` 里，已改为 `esp32s3`。

7. **出厂固件已被覆盖。** 本工程烧写的是自己的 app 分区（0x10000 起），
   原厂 demo 界面不再存在；需要时用微雪的固件包重新烧写。

8. **屏的可视区是圆形。** 面板是 360×360，但只有内切圆可见，四角被外壳遮住。
   排版要收在半径 ~165 的圆内，否则日期/状态行会落到看不见的区域。

9. **像素字节序要在软件里做一次交换。**
   厂商初始化序列里有 4 条 `0xC8`（RAMCTL），它固定了面板的字节序，会**覆盖**
   `esp_lcd_panel_dev_config_t.data_endian`，所以设成 `LITTLE` 也不起作用。
   现象：青色→黄色、灰色→深蓝、绿色→洋红（白色对称，看不出来）。
   现在在 [display.c](main/display.c) 的 `push_bitmap()` 里统一交换一次字节，
   所有绘制路径（填充、文字、圆环、行缓冲）都会生效。

10. **`display_draw_ring()` 会重绘整个圆形包围盒**（非环区域填背景色），
    所以圆环之后必须重画所有文字，否则日期/星期会被每秒一次的圆环刷新擦掉。

11. **这块板上的 RTC（PCF85063 @0x51）行为不标准**，不能作为时间来源：
    - 年份寄存器 `0x08` 写不进去（写 `0x26`、`0x42` 都读回 `0x06`/`0x02`，清 OS 标志后重写同样无效）
    - 偶发位错误（小时读到 `0x33`，或 00 读成 04）
    - 100kHz 降速后依旧

    因此表盘时间以 **SNTP 校时 + 软件时钟（`esp_timer`）** 为准，RTC 只做 best-effort 写入。
    代价是断电后时间从编译时间重来，开机约 5 秒内自动校准。

12. **BQ27220 的 StateOfCharge（0x2C）不可信**：未配置电池建模参数时，
    4.1V 会报 12%。电压寄存器（0x08）是准的，所以电量改为按电压曲线插值估算
    （4.2V=100%，3.3V=0%，见 [bq27220.c](main/bq27220.c) 的 `SOC_CURVE`）。

---

## 6. 目录结构

```
hello_world/
├── CMakeLists.txt                 顶层工程定义（MINIMAL_BUILD）
├── sdkconfig.defaults             可提交的默认配置（16MB flash + 自定义分区表）
├── partitions.csv                 分区表：app 分区 4MB（默认 1MB 已逼近上限）
├── sdkconfig                      本地生成配置（已 gitignore）
├── pytest_hello_world.py          原示例的自动化测试脚本
├── main/
│   ├── CMakeLists.txt             组件依赖清单
│   ├── main.c                     应用入口：初始化各外设并启动任务
│   ├── app_config.h               NTP / 时区 / 天气坐标 / 配网开关
│   ├── board_config.h             板级硬件配置（屏幕/触摸/I2C 引脚与参数）
│   │
│   ├── display.c / display.h      ST77916 面板驱动（初始化、填充、文本、圆环、行缓冲）
│   ├── font8x16.h                 8×16 ASCII 字模（生成）
│   ├── font16x16_zh.h             16×16 GB2312 中文字模（4437 字，生成）
│   ├── st77916_init_cmds.h        厂商初始化序列 v1 / v2（生成）
│   │
│   ├── ui.c / ui.h                UI 共享层：表盘枚举、共享状态、文本与矢量绘制原语
│   ├── faces.c / faces.h          七个表盘的绘制实现与分发
│   ├── snake.c / snake.h          贪吃蛇小游戏（独立模块）
│   ├── watch.c / watch.h          表盘主任务：软时钟、手势、切换、背光省电
│   │
│   ├── touch.c / touch.h          CST816S 触摸（基于官方 esp_lcd_touch）
│   ├── rtc.c / rtc.h              PCF85063 实时时钟
│   ├── bq27220.c / bq27220.h      BQ27220 电量计（电压估算电量）
│   ├── weather.c / weather.h      Open-Meteo 天气拉取与解析
│   ├── wifi_prov.c / wifi_prov.h  WiFi 配网（NVS 账号 + SoftAP 网页）
│   ├── wifi_time.c / wifi_time.h  WiFi + SNTP + 天气的后台任务
│   ├── imu.c / imu.h              QMI8658 加速度计
│   ├── pedometer.c / pedometer.h  计步算法（峰值检测 + NVS 按天保存）
│   ├── board_i2c.c / board_i2c.h  共享 I2C 总线
│   └── idf_component.yml          组件管理器清单（esp_lcd_st77916 / esp_lcd_touch / cst816s）
└── managed_components/            组件管理器下载的依赖（已 gitignore）
```

依赖组件：`espressif/esp_lcd_st77916` v2.0.2（从 ESP Component Registry 拉取）。

---

## 7. 下一步可做

- **IMU 手势**：QMI8658（I2C `0x6B`），抬腕亮屏 / 摇一摇换表盘
- **上 LVGL**：`lvgl/lvgl` + `espressif/esp_lvgl_port`，做更复杂的界面与动画
- **2048 / 俄罗斯方块**：再来一个小游戏，复用贪吃蛇的增量渲染框架
- **音乐播放**：SD 卡 MP3 → ES8311 输出
- **恢复出厂界面**：用微雪提供的固件包重新烧写

---

## 8. 参考

- [微雪 ESP32-S3-Touch-LCD-1.85B 文档](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B)
- [官方示例仓库](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B)
  （`Examples/ESP-IDF-V5.5.3/` 下是 BSP 与各示例）
- [espressif/esp_lcd_st77916 组件](https://components.espressif.com/components/espressif/esp_lcd_st77916)
- [ESP-IDF v6.1 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/index.html)

> 本工程源自 ESP-IDF 的 `hello_world` 示例（CC0-1.0）。
> ST77916 初始化序列来自微雪 BSP（Apache-2.0）。
