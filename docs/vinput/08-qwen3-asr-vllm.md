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
    --uds $XDG_RUNTIME_DIR/vllm.sock \
    --gpu-memory-utilization 0.8
```

首次运行会自动下载模型 (~3.5GB)。服务启动后保持运行。

## 验证

```bash
curl --unix-socket $XDG_RUNTIME_DIR/vllm.sock -s http://localhost/v1/models | head -c 200
```

## 架构

```
fcitx5 → CapsLock 长按 → adapter 激活
  → Qwen3AsrProvider::start()
    → PulseAudio 录音 (16kHz 16bit mono)
  → CapsLock 松开 → stop()
    → WAV base64 → POST /tmp/vllm.sock (UDS)
    → vLLM 推理 → JSON response
    → commitString(text)
```

## 配置

Unix socket 路径默认 `/tmp/vllm.sock`。
