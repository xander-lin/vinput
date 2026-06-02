# Qwen3-ASR (vLLM) 后端设置

## 安装

```bash
# 创建 Python 虚拟环境
uv venv ~/vllm-env
source ~/vllm-env/bin/activate
uv pip install vllm qwen-asr --pre \
    --extra-index-url https://wheels.vllm.ai/nightly/cu129
```

## 启动 vLLM 服务

```bash
source ~/vllm-env/bin/activate
vllm serve Qwen/Qwen3-ASR-1.7B \
    --host 127.0.0.1 --port 8000 \
    --gpu-memory-utilization 0.8
```

首次运行会自动下载模型 (~3.5GB)。服务启动后保持运行。

## 验证

```bash
curl -s http://127.0.0.1:8000/v1/models | head -c 200
```

## 架构

```
fcitx5 → CapsLock 长按 → adapter 激活
  → Qwen3AsrProvider::start()
    → PulseAudio 录音 (16kHz 16bit mono)
  → CapsLock 松开 → stop()
    → WAV base64 → POST /v1/chat/completions
    → vLLM 推理 → JSON response
    → commitString(text)
```

## 配置

在 adapter 配置中可指定 vLLM 地址，默认 `http://127.0.0.1:8000`。
