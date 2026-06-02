# Qwen3-ASR (vLLM) 后端设置

## 安装

```bash
# 创建 Python 虚拟环境
uv venv ~/vllm-env
source ~/vllm-env/bin/activate
uv pip install vllm qwen-asr --pre \
    --extra-index-url https://wheels.vllm.ai/nightly/cu129
```

## 下载模型

```bash
source ~/vllm-env/bin/activate
mkdir -p ~/.local/share/vinput/models

# ModelScope (国内)
modelscope download --model Qwen/Qwen3-ASR-1.7B \
    --local_dir ~/.local/share/vinput/models/Qwen3-ASR-1.7B
```

## 安装 systemd 服务 (开机自启 + 懒加载)

```bash
mkdir -p ~/.config/systemd/user
cp systemd/vllm-qwen3.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable vllm-qwen3.service
```

服务不会立即启动——adapter 首次连接 socket 时会自动拉起（轮询等待最多 30s），空闲时不占 GPU。

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
