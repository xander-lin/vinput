# fcitx5-vinput

Voice input addon for [fcitx5](https://github.com/fcitx/fcitx5). Push-to-talk speech recognition via CapsLock.

## Usage

| Action | Keys |
|--------|------|
| Voice input | Hold **CapsLock** → speak → release |
| Switch ASR provider | **Ctrl+CapsLock**, then **←/→** |
| Switch denoiser | **Ctrl+CapsLock**, then **↑/↓** |

## ASR Backends

| Provider | Type | Requires |
|----------|------|----------|
| Mock | Test | Nothing |
| Zipformer | Local (~100MB) | `sherpa-onnx` binary |
| FireRed | Local (~1.2GB) | `sherpa-onnx-offline` binary |
| Doubao | Cloud (ByteDance) | API key |
| Qwen | Cloud (Alibaba) | API key |

## Quick Start

### 1. Install

```bash
# AUR (Arch Linux)
yay -S fcitx5-vinput-git

# Manual build
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

### 2. Configure (cloud ASR)

```bash
mkdir -p ~/.config/vinput

# Doubao
echo '{"api_key":"xxx","resource_id":"volc.seedasr.auc"}' > ~/.config/vinput/doubao.json

# Qwen
echo '{"api_key":"sk-xxx"}' > ~/.config/vinput/qwen.json

# Desktop environment (for window focus switching)
echo '{"desktop":"niri"}' > ~/.config/vinput/output.json
```

### 3. Restart fcitx5

```bash
fcitx5 -r
```

## Local Models (Optional)

Local models require [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) binaries and model files.

### 1. Install sherpa-onnx binaries

Download the latest Linux release from [sherpa-onnx releases](https://github.com/k2-fsa/sherpa-onnx/releases):

```bash
# Example for v1.13.x (adjust version as needed)
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.2/sherpa-onnx-v1.13.2-linux-x64.tar.bz2
tar xf sherpa-onnx-v1.13.2-linux-x64.tar.bz2
mkdir -p ~/.local/share/vinput/sherpa-onnx
cp -r sherpa-onnx-v1.13.2-linux-x64/bin ~/.local/share/vinput/sherpa-onnx/
```

### 2. Download ASR models

| Model | Size | Download |
|-------|------|----------|
| Zipformer (zh-en) | ~100MB | `sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2` |
| FireRed AED (zh-en) | ~1.2GB | `sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2` |

```bash
mkdir -p ~/.local/share/vinput/models

# Zipformer
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2
tar xf sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2 -C ~/.local/share/vinput/models/

# FireRed
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 -C ~/.local/share/vinput/models/
```

Check [sherpa-onnx ASR models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models) for the latest model URLs.

## Configuration

All config in `~/.config/vinput/`. See man page: `man vinput`

### User-facing (must configure)

| File | Purpose | Example |
|------|---------|---------|
| `doubao.json` | Doubao API credentials | `{"api_key":"...","resource_id":"..."}` |
| `qwen.json` | Qwen API key | `{"api_key":"sk-..."}` |
| `output.json` | Desktop environment | `{"desktop":"niri"}` |
| `vinput.json` | Interaction tuning | `{"activation_msec":300}` |

### Advanced (optional, all defaults in code)

| File | Purpose |
|------|---------|
| `advanced.json` | All tunables: model paths, thread counts, timeouts, audio params |
| `audio.json` | Denoiser: `"none"` \| `"speexdsp"` \| `"deepfilter"` |

## Dependencies

| Library | Arch Package |
|---------|-------------|
| fcitx5 | `fcitx5` |
| libebur128 | `libebur128` |
| libpulse | `libpulse` |
| libcurl | `curl` |
| speexdsp | `speexdsp` |
| libsoxr | `libsoxr` |

## Audio Pipeline

```
PulseAudio → ebur128 norm (-16 LUFS) → denoise → VAD trim → WAV → ASR → commit
```

## License

MIT
