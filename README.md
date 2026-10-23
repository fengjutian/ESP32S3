# hello_world — ESP32-S3-Touch-LCD-1.85B

基于 ESP-IDF 的 `hello_world` 示例改造：**串口打印 + 屏幕显示文字**，运行在
微雪 (Waveshare) **ESP32-S3-Touch-LCD-1.85B** 开发板上。

程序不再无限重启，而是常驻运行：开机画一次标题，之后每 2 秒刷新一行运行状态。

```
Hello World        ← 白色，3 倍字（24x48 像素）
ESP32-S3           ← 青色，2 倍字
up 12s   heap 362K ← 绿色，每 2 秒刷新
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
I (289) display: panel reg 0x04 = 00 02 7F 7F
I (299) display: using init sequence v2 (185 commands)
I (569) display: panel ready: 360x360 (ST77916 QSPI, v2)
Heartbeat #0: uptime 600 ms, free heap 370644 bytes
```

---

## 4. 屏幕相关代码

| 文件 | 作用 |
|---|---|
| [main/display_config.h](main/display_config.h) | **硬件配置**：分辨率、QSPI 引脚、时钟、初始化版本、镜像/反色 |
| [main/display.c](main/display.c) | ST77916 初始化 + 填充 + 文本绘制（DMA 分行刷屏） |
| [main/display.h](main/display.h) | API 与 RGB565 颜色宏 |
| [main/st77916_init_cmds.h](main/st77916_init_cmds.h) | 厂商 v1（182 条）/ v2（185 条）初始化序列 |
| [main/font8x16.h](main/font8x16.h) | 8×16 ASCII 点阵字模（字符 32~126，Consolas 14px 生成） |
| [main/hello_world_main.c](main/hello_world_main.c) | `app_main`：串口打印 + 屏幕内容 + 心跳循环 |

### API 用法

```c
#include "display.h"
#include "display_config.h"

display_init();                                   // 失败会返回错误码，不影响串口部分

display_clear(COLOR_BLACK);                       // 整屏填充
display_fill_rect(0, 260, 360, 20, COLOR_BLACK);  // 局部填充（会自动裁剪）

display_draw_text(48, 96, "Hello World", 3,       // x, y, 文本, 放大倍数
                  COLOR_WHITE, COLOR_BLACK);      // 前景色, 背景色

int w = display_text_width("Hello World", 3);     // 计算宽度，便于居中
```

坐标系原点在左上角，x 向右、y 向下。文本超出屏幕会自动裁剪。

### 显示效果微调

改 [main/display_config.h](main/display_config.h)，改完重新编译烧录即可：

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

---

## 6. 目录结构

```
hello_world/
├── CMakeLists.txt                 顶层工程定义（MINIMAL_BUILD）
├── sdkconfig.defaults             可提交的默认配置（16MB flash）
├── sdkconfig                      本地生成配置（已 gitignore）
├── pytest_hello_world.py          原示例的自动化测试脚本
├── main/
│   ├── CMakeLists.txt             组件依赖：esp_lcd / esp_driver_spi / esp_driver_gpio / esp_lcd_st77916
│   ├── hello_world_main.c         应用入口
│   ├── display.c / display.h      ST77916 显示驱动
│   ├── display_config.h           硬件配置（引脚、分辨率、时钟）
│   ├── st77916_init_cmds.h        厂商初始化序列 v1 / v2
│   ├── font8x16.h                 8×16 ASCII 字模
│   └── idf_component.yml          组件管理器清单（espressif/esp_lcd_st77916 ^2.0.2）
└── managed_components/            组件管理器下载的依赖（已 gitignore）
```

依赖组件：`espressif/esp_lcd_st77916` v2.0.2（从 ESP Component Registry 拉取）。

---

## 7. 下一步可做

- **触摸交互**：接 CST816S（I2C `0x15`，SCL=10 / SDA=11 / INT=4 / RST=1），触摸后换文字
- **显示中文**：当前字模只有 ASCII，需要 GB2312 / 点阵字库生成
- **上 LVGL**：`lvgl/lvgl` + `espressif/esp_lvgl_port`，做图形界面
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
