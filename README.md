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
paru -S fcitx5-vinput-git

# Manual build
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

### 2. Configure (cloud ASR)

The package installs default config files to `/etc/vinput/`. Existing edited files under `/etc/vinput/` are preserved by pacman on upgrade.

Per-user files under `~/.config/vinput/` override `/etc/vinput/`. Missing per-user config files are created from `/etc/vinput/` when Vinput first reads them.

```bash
mkdir -p ~/.config/vinput

# Copy tracked examples, then edit local files under ~/.config/vinput/
cp config/doubao.json.example ~/.config/vinput/doubao.json
cp config/qwen.json.example ~/.config/vinput/qwen.json
cp config/output.json.example ~/.config/vinput/output.json
cp config/audio.json.example ~/.config/vinput/audio.json
cp config/vinput.json.example ~/.config/vinput/vinput.json
cp config/advanced.json.example ~/.config/vinput/advanced.json

# Doubao and Qwen need API keys before use
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

Config is loaded from `~/.config/vinput/` first, then `/etc/vinput/`. If a user config file is missing and the packaged `/etc/vinput/` file exists, Vinput copies it to `~/.config/vinput/` without overwriting existing files. See man page: `man vinput`

Tracked files under `config/*.json.example` are examples only. Runtime `*.json` files are ignored by Git and should stay under `~/.config/vinput/`.

### User-facing (must configure)

| File | Purpose | Example |
|------|---------|---------|
| `doubao.json` | Doubao API credentials | copy from `config/doubao.json.example` |
| `qwen.json` | Qwen API key | copy from `config/qwen.json.example` |
| `output.json` | Desktop environment | copy from `config/output.json.example` |
| `vinput.json` | Interaction tuning | copy from `config/vinput.json.example` |

### Advanced (optional, all defaults in code)

| File | Purpose |
|------|---------|
| `advanced.json` | All tunables: model paths, thread counts, timeouts, audio params, copy from `config/advanced.json.example` |
| `audio.json` | Denoiser: `"none"` \| `"speexdsp"` \| `"deepfilter"`, copy from `config/audio.json.example` |

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
