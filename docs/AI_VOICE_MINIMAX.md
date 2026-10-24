# 语音问答接入（MiniMax）

手表上按住屏幕说话 → 云端识别 → 大模型回答 → 语音合成播报。
整条链路都在设备端完成，不需要自建服务器。

```
  ┌──────────── ESP32-S3 (本仓库) ────────────┐        ┌──────── MiniMax 开放平台 ────────┐
  │                                           │        │                                  │
  │  ES7210 双麦 ──I2S──► 16k PCM 缓冲(PSRAM) │        │                                  │
  │        │                     │            │  HTTPS │  POST /v1/speech_to_text  (ASR)  │
  │        │              打包成 WAV           │ ─────► │        └─► 文本                   │
  │        │                     │            │        │                                  │
  │        │              大模型请求            │ ─────► │  POST /v1/chat/completions       │
  │        │                     │            │        │        └─► 回答文本               │
  │        │              TTS 请求             │ ─────► │  POST /v1/t2a_v2                 │
  │        ▼                     ▼            │        │        └─► hex PCM (16k 单声道)   │
  │   ES8311 ◄──I2S──  PCM 16k ◄── hex 解码 ◄─┘        └──────────────────────────────────┘
  │        │
  │     PA(GPIO9) ──► 喇叭
  └───────────────────────────────────────────┘
```

## 1. 用到的 MiniMax 接口

| 环节 | 接口 | 关键参数 |
|---|---|---|
| 语音识别 | `POST /v1/speech_to_text` | multipart，`model=asr-1.0`，文件为 wav/mp3（≤500 秒、≤50MB） |
| 大模型 | `POST /v1/chat/completions` | `model=MiniMax-M3.1-Flash`，OpenAI 兼容格式 |
| 语音合成 | `POST /v1/t2a_v2` | `model=speech-2.8-turbo`，`output_format=hex`，`audio_setting.format=pcm` |

鉴权统一是 `Authorization: Bearer <API Key>`。

网关按账号区域二选一，**key 和区域绑定**：

- 海外：`https://api.minimax.io`
- 国内：`https://api.minimax.cn`

> 老教程里的 `GroupId` 参数已经不需要了，现在的接口用 API Key 鉴权。

### 1.1 语音识别

```http
POST /v1/speech_to_text
Authorization: Bearer sk-api-xxx
language: zh
Content-Type: multipart/form-data; boundary=...

--boundary
Content-Disposition: form-data; name="model"

asr-1.0
--boundary
Content-Disposition: form-data; name="file"; filename="q.wav"
Content-Type: audio/wav

<WAV 二进制>
--boundary--
```

返回：`{"text": "今天天气怎么样", "duration": 2.31, "trace_id": "..."}`

要点：单声道 16kHz 足够，**不要**传立体声 48k（体积涨 6 倍还更慢）。

### 1.2 大模型

```json
{
  "model": "MiniMax-M3.1-Flash",
  "messages": [
    {"role": "system", "content": "你是一块智能手表的语音助手，回答控制在80字以内……"},
    {"role": "user", "content": "今天天气怎么样"}
  ],
  "max_completion_tokens": 320,
  "stream": false,
  "reasoning_effort": "low"
}
```

返回取 `choices[0].message.content`。

注意点：

- **M3.1-Flash 总是「思考」**，思考内容单独放在 `reasoning_content`，`content` 才是答案；
- `reasoning_effort` 可调 `low/medium/high/xhigh/max`，语音场景建议 `low`（默认 `max` 会很慢）；
- 错误同时在 HTTP 状态和 `base_resp.status_code` 里，两个都要看。

### 1.3 语音合成

```json
{
  "model": "speech-2.8-turbo",
  "text": "今天南京多云，18 到 24 度。",
  "stream": false,
  "output_format": "hex",
  "voice_setting": {"voice_id": "Chinese (Mandarin)_Lyrical_Voice", "speed": 1.0, "vol": 1.0, "pitch": 0},
  "audio_setting": {"sample_rate": 16000, "format": "pcm", "channel": 1},
  "language_boost": "Chinese"
}
```

返回 `data.audio` 是十六进制字符串，解开就是裸 PCM（s16le、16kHz、单声道），
不用解 mp3、也不用额外下载文件——这是设备端最省事的组合。

## 2. 代码结构

| 文件 | 作用 |
|---|---|
| `main/net_link.c/h` | WiFi 会话管理：引用计数 + 空闲 60 秒后关 WiFi |
| `main/minimax.c/h` | 三个云端接口 + 极简 JSON 解析 + hex 原地解码 |
| `main/audio_io.c/h` | I2S 全双工 + ES8311/ES7210 初始化、录音/播放 |
| `main/voice_ai.c/h` | 状态机：录音 → ASR → 对话 → TTS → 播放；AI 表盘绘制 |
| `main/ai_config.h` | 模型、音色、录音时长、麦克风增益等可调参数 |
| `main/app_config.h` | **API Key 写这里**（已在 .gitignore） |
| `tools/minimax_check.py` | PC 端自检脚本，先用它验证 key/模型/音色 |

### 2.1 一次问答的时序

| 阶段 | 耗时（实测参考） | 说明 |
|---|---|---|
| WiFi 重连 | 1~3 s（仅冷启动第一次） | 之后 60 秒内保持在线，连续追问无感 |
| 录音 | 用户决定 | 上限 8 秒（`AI_MAX_RECORD_MS`） |
| ASR 上传+识别 | 1.5~3 s | 256KB WAV |
| 大模型 | 2~5 s | `low` 思考深度 |
| TTS + 下发 | 1~2 s | 回答越长越久 |
| 播放 | 实时 | 边收到边解码，收到即可播 |

端到端大约 **5~9 秒**，主要瓶颈是大模型的思考过程。

### 2.2 内存

- 录音缓冲：`8s × 16k × 2B ≈ 256KB`，放 **PSRAM**；
- TTS 响应：最多 3MB（hex 是 PCM 的两倍），放 PSRAM，**原地解码**，不额外拷贝；
- 因此 `sdkconfig.defaults` 里打开了 Octal PSRAM：

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_IGNORE_NOTFOUND=y
```

没有 PSRAM 的话，8 秒录音会直接把内部 512KB SRAM 吃掉一半。

## 3. 配置与烧录

### 3.1 填 API Key

```c
/* main/app_config.h */
#define MINIMAX_API_KEY   "sk-api-xxxxxxxx"
#define MINIMAX_BASE_URL  "https://api.minimax.io"   // 国内账号改成 api.minimax.cn
```

`app_config.h` 在 `.gitignore` 里，不会提交。`app_config.example.h` 有模板。

### 3.2 先跑 PC 自检

```powershell
pip install requests
$env:MINIMAX_API_KEY="sk-api-xxxx"
python tools/minimax_check.py chat "用一句话介绍你自己"
python tools/minimax_check.py tts "你好，我是手表助手" out.wav
python tools/minimax_check.py asr question.wav
```

三段都通了再烧板子，能排除 90% 的「key 不对 / 区域不对 / 模型名不对」。

### 3.3 编译烧录

```powershell
$env:IDF_TOOLS_PATH="C:\Espressif\tools"
& C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py build
idf.py -p COM5 flash monitor
```

> 改动 PSRAM 配置后必须 `idf.py fullclean` 再编译，否则链接脚本还是旧的。

### 3.4 使用

- 左右滑动切到 **「语音」** 表盘；
- **按住屏幕说话**，松手开始识别；
- 回答会显示在屏幕上，同时从喇叭读出；
- 轻点一下（<0.5 秒）= **重播上一次回答**；
- **处理中再按一下屏幕 = 打断**（正在播放/思考时立即停下）；
- 左右滑动 = 切走（会中断当前请求）；
- 状态行会显示已等待秒数（`识别中… 2s`），一眼能看出没卡住。

### 3.5 延迟（实测）

| 阶段 | 优化前 | 优化后 |
|---|---|---|
| 语音合成出声 | 5.3 s（等整段） | **1.4 s**（流式，边收边播） |
| 回答长度 | 50~80 字，播 10~16 s | ≤40 字，播 4~6 s |
| 打通到出声（云问答） | 约 14 s | 约 8~9 s |
| 打通到出声（本地意图） | 约 14 s | **约 4 s** |

识别（约 2.4 s）和大模型（约 2~3 s）是云端处理时间，属于硬成本；
再往下压就要做「边识别边送大模型」或本地意图分流。

### 3.6 本地意图

下面这些问法**不走大模型**，识别完直接本地拼答案（省一次请求、省 token、快 2~3 秒）：

| 意图 | 触发词（示例） | 数据来源 |
|---|---|---|
| 时间 | 几点、时间、时刻 | 表盘软件时钟（SNTP 校准） |
| 日期 | 几号、日期、星期、礼拜、几月 | 同上 |
| 天气 | 天气、气温、温度、下雨、冷不冷、热不热 | Open-Meteo 缓存（30 分钟一次） |
| 电量 | 电量、电池、多少电、还有电、几格电 | BQ27220 |
| 步数 | 步数、走了多少、几步 | 计步器 |
| 身份/客套 | 你是谁、你叫什么、谢谢、再见 | 固定话术 |

匹配不上（比如"讲个笑话"）才走云端。实现见 `main/local_intent.c`。

## 4. 关键实现说明

### 4.1 一路 I2S 带两个 codec

板子上 ES8311（DAC）和 ES7210（ADC）共用 `MCLK=2 / BCLK=48 / LRCK=38`，
所以只能起**一个** I2S 控制器，TX 当时钟主机、开全双工：

```c
i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
i2s_channel_init_std_mode(s_tx, &std_cfg);   // 先 TX，它出时钟
i2s_channel_init_std_mode(s_rx, &std_cfg);
```

然后一个 `audio_codec_new_i2s_data()` 同时挂两个 codec 设备：

```c
esp_codec_dev_cfg_t out = { ESP_CODEC_DEV_TYPE_OUT, es8311, data_if };
esp_codec_dev_cfg_t in  = { ESP_CODEC_DEV_TYPE_IN,  es7210, data_if };
```

采样固定 16kHz/16bit/双声道：ES7210 双麦出左右声道，录音取左声道；
ES8311 是单声道，播放时把一个样本复制到左右两个 slot。

### 4.2 PSRAM 上做 hex 解码

`data.audio` 是 JSON 里的一整段十六进制。做法是**同一块缓冲区原地解码**：
目标位置永远在源位置之前，所以可以前向覆盖，省掉一次 3MB 的拷贝。

### 4.3 WiFi 不再"用完就关"

原来 `wifi_time.c` 是「连上 → 校时 → 拉天气 → 立刻关」。
语音问答一轮要连发三个请求，反复重连会多花 2~3 秒，
所以抽出了 `net_link`：引用计数归零后**再保持 60 秒**才关。
天气刷新仍然是每 30 分钟一次，行为不变。

### 4.4 交互与任务划分

- 表盘任务（`watch.c`）只做手势判定和绘制，**不碰网络和 I2S**；
- 语音任务（`voice_ai.c`）串行执行整条链路，20KB 栈（TLS 握手吃栈）；
- 两边共享的文字用互斥锁保护，绘制只在表盘任务里发生。

## 5. 成本与优化方向

按官方价目，一轮问答大约：

| 项目 | 用量 | 说明 |
|---|---|---|
| ASR | 3~8 秒音频 | 按音频时长计费 |
| LLM | 约 100~400 token | 系统提示词 + 短回答 |
| TTS | 50~100 字 | 按字符计费 |

想更快/更省，按性价比排序：

1. **流式 TTS**：`/v1/t2a_v2` 支持 `stream=true`（SSE），首包 300ms 级，
   可以边收边播；设备端需要把 hex chunk 逐个解码送 I2S；
2. **WebSocket 双向流式**：`speech-t2a-websocket-bidi`，适合把大模型的流式输出直接灌进去；
3. **回答更短**：把系统提示词压到 40 字以内、`AI_CHAT_MAX_TOKENS` 降到 160；
4. **本地 VAD**：检测到静音自动结束录音，用户不用一直按着；
5. **缓存常见问答**：天气/时间这类问题直接在本地答，根本不用上云。

## 6. 安全提醒

API Key 会被编译进固件，能从 flash 里读出来。个人玩具没问题，但：

- 不要把带 key 的固件发到网上；
- 如果要量产/公开，**加一层自己的代理服务**：设备只带一个短期 token，
  由服务端持有 MiniMax key，顺便还能做限流、计费和内容审核。

## 7. 排错

| 现象 | 原因 / 处理 |
|---|---|
| 界面提示「未配置 API Key」 | `app_config.h` 里没填 `MINIMAX_API_KEY` |
| HTTP 401 / 404 | key 与网关区域不匹配。`sk-cp-` 开头的订阅 key 一般要用 `https://api.minimax.cn`；`invalid api key (2049)` 就是区域或 key 本身就错了 |
| `base_resp.status_code=1004` | 鉴权失败，key 抄错或已禁用 |
| `1039` | 触发限流，稍后再试 |
| `1008` | 余额不足 |
| 识别返回空文本 | 录音太短；或麦克风选错，把 `AI_MIC_SELECT` 从 `0x03` 改成 `0x0C` |
| 有文字没声音 | 检查 `GPIO9` 功放、喇叭焊盘；把 `AI_SPEAKER_VOLUME` 调到 100 |
| 播放是杂音/快放 | TTS 输出的采样率和 `AI_SAMPLE_RATE` 不一致 |
| 开机就 panic / PSRAM 报错 | 这块板是 **Octal** PSRAM，确认 `CONFIG_SPIRAM_MODE_OCT=y`；改完 `idf.py fullclean` |
| 编译报 `undefined reference to es8311_codec_new` | 组件没拉到，删掉 `managed_components/` 和 `dependencies.lock` 重新 `idf.py reconfigure` |

## 8. 参考

- 平台文档：<https://platform.minimax.io/docs>
- 语音识别：<https://platform.minimax.io/docs/api-reference/speech-to-text>
- 同步 TTS：<https://platform.minimax.io/docs/api-reference/speech-t2a-http>
- Chat Completions：<https://platform.minimax.io/docs/api-reference/text-chat-openai>
- 模型列表（含下架时间）：<https://platform.minimax.io/docs/faq/models>
- 板子引脚：<https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B/>
