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

### Prepare directory layout

```bash
mkdir -p ~/.local/share/vinput/{sherpa-onnx/bin,models}
```

Expected structure after setup:
```
~/.local/share/vinput/
├── sherpa-onnx/bin/
│   ├── sherpa-onnx          # Zipformer (streaming transducer)
│   └── sherpa-onnx-offline  # FireRed (offline AED)
└── models/
    ├── zipformer-zh-en/
    │   ├── encoder-epoch-99-avg-1.onnx
    │   ├── decoder-epoch-99-avg-1.onnx
    │   ├── joiner-epoch-99-avg-1.onnx
    │   └── tokens.txt
    └── sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26/
        ├── encoder.int8.onnx
        ├── decoder.int8.onnx
        └── tokens.txt
```

### 1. Download & extract sherpa-onnx binaries

```bash
# Find latest version at: https://github.com/k2-fsa/sherpa-onnx/releases
VERSION=v1.13.2
ARCHIVE="sherpa-onnx-${VERSION}-linux-x64.tar.bz2"
curl -LO "https://github.com/k2-fsa/sherpa-onnx/releases/download/${VERSION}/${ARCHIVE}"
tar xf "$ARCHIVE"
cp -r "sherpa-onnx-${VERSION}-linux-x64/bin/"* ~/.local/share/vinput/sherpa-onnx/bin/

# Clean up
rm -rf "sherpa-onnx-${VERSION}-linux-x64" "$ARCHIVE"
```

### 2. Download & extract ASR models

| Model | Size | Cold Start | Archive |
|-------|------|------------|---------|
| Zipformer zh-en | ~100MB | ~1s | `sherpa-onnx-zipformer-zh-en-2023-06-26` |
| FireRed AED zh-en | ~1.2GB | ~3s | `sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26` |

```bash
MO="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"

# Zipformer (real-time streaming transducer)
curl -LO "$MO/sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2"
tar xf sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2 -C ~/.local/share/vinput/models/

# FireRed (offline AED, higher accuracy)
curl -LO "$MO/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 -C ~/.local/share/vinput/models/

# Clean up
rm *.tar.bz2
```

> Model archives are at [sherpa-onnx ASR models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models).  
> Custom paths can be set in `~/.config/vinput/advanced.json`.


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
