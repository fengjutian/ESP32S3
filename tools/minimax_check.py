#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MiniMax 语音问答 —— PC 端自检脚本。

在烧板子之前，先用它确认「key / 区域 / 模型 / 音色」都是通的，
免得在设备上摸黑调试。

用法：

    # 1. 只测大模型
    set MINIMAX_API_KEY=sk-api-xxxx
    python tools/minimax_check.py chat "用一句话介绍你自己"

    # 2. 文本转语音（存到 out.wav）
    python tools/minimax_check.py tts "你好，我是手表上的语音助手" out.wav

    # 3. 语音识别（wav 需 16k 单声道）
    python tools/minimax_check.py asr question.wav

    # 4. 一条龙：录音 5 秒 → 识别 → 大模型 → 合成 → 播放
    python tools/minimax_check.py talk --seconds 5

国内账号加 --cn，或设置 MINIMAX_BASE_URL=https://api.minimax.cn

依赖：requests（必装）；录音/播放需要 sounddevice + numpy（可选）。
"""

import argparse
import json
import os
import sys
import wave

try:
    import requests
except ImportError:
    sys.exit("请先安装依赖：pip install requests")

BASE = os.environ.get("MINIMAX_BASE_URL", "https://api.minimax.io")
CHAT_MODEL = os.environ.get("MINIMAX_CHAT_MODEL", "MiniMax-M3.1-Flash")
TTS_MODEL = os.environ.get("MINIMAX_TTS_MODEL", "speech-2.8-turbo")
TTS_VOICE = os.environ.get("MINIMAX_TTS_VOICE", "Chinese (Mandarin)_Lyrical_Voice")
ASR_MODEL = os.environ.get("MINIMAX_ASR_MODEL", "asr-1.0")

SYSTEM_PROMPT = (
    "你是一块智能手表的语音助手。回答用口语化的中文，"
    "控制在80字以内，不要用 Markdown、列表或表情符号。"
)


def session(key: str) -> requests.Session:
    s = requests.Session()
    s.headers.update({"Authorization": f"Bearer {key}"})
    return s


def check(resp: requests.Response) -> dict:
    """统一处理 HTTP 错误和 MiniMax 的 base_resp。"""
    if resp.status_code != 200:
        sys.exit(f"HTTP {resp.status_code}: {resp.text[:400]}")
    body = resp.json()
    base_resp = body.get("base_resp") or {}
    code = base_resp.get("status_code", 0)
    if code not in (0, None):
        sys.exit(f"MiniMax 返回错误 {code}: {base_resp.get('status_msg')}")
    return body


def chat(s: requests.Session, text: str) -> str:
    payload = {
        "model": CHAT_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "max_completion_tokens": 320,
        "stream": False,
        "reasoning_effort": "low",
    }
    r = s.post(f"{BASE}/v1/chat/completions", json=payload, timeout=60)
    body = check(r)
    return body["choices"][0]["message"]["content"]


def tts(s: requests.Session, text: str, out_path: str) -> None:
    payload = {
        "model": TTS_MODEL,
        "text": text,
        "stream": False,
        "output_format": "hex",
        "voice_setting": {"voice_id": TTS_VOICE, "speed": 1.0, "vol": 1.0, "pitch": 0},
        "audio_setting": {"sample_rate": 16000, "format": "pcm", "channel": 1},
        "language_boost": "Chinese",
    }
    r = s.post(f"{BASE}/v1/t2a_v2", json=payload, timeout=120)
    body = check(r)
    audio_hex = (body.get("data") or {}).get("audio")
    if not audio_hex:
        sys.exit(f"没有返回音频：{json.dumps(body)[:300]}")

    pcm = bytes.fromhex(audio_hex)
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)
    print(f"已保存 {out_path}（{len(pcm) / 32000:.1f} 秒）")


def asr(s: requests.Session, wav_path: str) -> str:
    with open(wav_path, "rb") as f:
        files = {"file": ("q.wav", f, "audio/wav")}
        data = {"model": ASR_MODEL}
        r = s.post(f"{BASE}/v1/speech_to_text", headers={"language": "zh"},
                   data=data, files=files, timeout=120)
    body = check(r)
    return body.get("text", "")


def record(seconds: float) -> str:
    """用 sounddevice 录 16k 单声道，返回临时 wav 路径。"""
    try:
        import numpy as np
        import sounddevice as sd
    except ImportError:
        sys.exit("录音需要：pip install sounddevice numpy")

    print(f"录音 {seconds:.0f} 秒，请说话…")
    audio = sd.rec(int(seconds * 16000), samplerate=16000, channels=1, dtype="int16")
    sd.wait()
    path = "_question.wav"
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(np.asarray(audio).tobytes())
    return path


def play(wav_path: str) -> None:
    try:
        import numpy as np
        import sounddevice as sd
    except ImportError:
        print(f"（未安装 sounddevice，音频在 {wav_path}）")
        return

    with wave.open(wav_path, "rb") as w:
        data = w.readframes(w.getnframes())
    sd.play(np.frombuffer(data, dtype=np.int16), samplerate=16000)
    sd.wait()


def main() -> None:
    global BASE

    ap = argparse.ArgumentParser(description="MiniMax 语音问答自检")
    ap.add_argument("--cn", action="store_true", help="使用国内网关 api.minimax.cn")
    ap.add_argument("--key", default=os.environ.get("MINIMAX_API_KEY", ""))
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_chat = sub.add_parser("chat")
    p_chat.add_argument("text")

    p_tts = sub.add_parser("tts")
    p_tts.add_argument("text")
    p_tts.add_argument("out", nargs="?", default="out.wav")

    p_asr = sub.add_parser("asr")
    p_asr.add_argument("wav")

    p_talk = sub.add_parser("talk")
    p_talk.add_argument("--seconds", type=float, default=5.0)

    args = ap.parse_args()

    if not args.key:
        sys.exit("缺少 API Key：设置环境变量 MINIMAX_API_KEY 或传 --key")

    if args.cn:
        BASE = "https://api.minimax.cn"

    s = session(args.key)
    print(f"网关 {BASE} / 对话模型 {CHAT_MODEL} / 音色 {TTS_VOICE}\n")

    if args.cmd == "chat":
        print(chat(s, args.text))
    elif args.cmd == "tts":
        tts(s, args.text, args.out)
    elif args.cmd == "asr":
        print(asr(s, args.wav))
    elif args.cmd == "talk":
        wav = record(args.seconds)
        question = asr(s, wav)
        print(f"你：{question}")
        answer = chat(s, question)
        print(f"AI：{answer}")
        out = "_answer.wav"
        tts(s, answer, out)
        play(out)


if __name__ == "__main__":
    main()
