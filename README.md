# fcitx5-vinput

Voice input addon for [fcitx5](https://github.com/fcitx/fcitx5). Push-to-talk speech recognition via CapsLock.

## Usage

| Action | Keys |
|--------|------|
| Voice input | Hold **CapsLock** → speak → release |
| Switch ASR provider | **Ctrl+CapsLock**, then **←/→** |
| Switch denoiser | **Ctrl+CapsLock**, then **↑/↓** |

### Recognition status and consecutive speech

The input panel beside the cursor reports the current phase:

```
listening → processing audio → recognizing → result or an actionable error
```

You may begin the next recording while an earlier cloud request is still
recognizing. Vinput preserves capture and commit order, and binds each result
to the application and input context active when that recording began.

To prevent unbounded memory and service requests, at most three recognitions
(including the active request) are accepted at once. When the queue is full,
Vinput shows `recognition queue full; try again` and discards only the newest
recording.

Cloud failures are shown in the input panel as actionable errors, one per
failure path: missing API key, rejected API key, model not found, local
engine setup (missing binary/model files), network, service unavailable,
timeout, no speech, audio capture, and uncategorized failures (which show
the provider's raw detail). These messages are status UI, not text inserted
into the application.

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
cp config/vinput.json.example ~/.config/vinput/vinput.json
cp config/audio.json.example ~/.config/vinput/audio.json
cp config/qwen.json.example ~/.config/vinput/qwen.json
cp config/doubao.json.example ~/.config/vinput/doubao.json

# Cloud providers: put your API key into the JSON (plaintext; chmod 600).
# Every config file is validated on read — typos and syntax errors are
# reported in the switch notification and the input panel.
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
> Custom paths can be set in `~/.config/vinput/zipformer.json` and `~/.config/vinput/fire_red.json`.


## Configuration

Config is loaded from `~/.config/vinput/` first, then `/etc/vinput/`. If a user config file is missing and the packaged `/etc/vinput/` file exists, Vinput copies it to `~/.config/vinput/` without overwriting existing files. See man page: `man vinput`

Tracked files under `config/*.json.example` are examples only. Runtime `*.json` files are ignored by Git and should stay under `~/.config/vinput/`.

Every provider config is re-read before each recognition, so model and parameter changes apply on the next recording without restarting fcitx5.

### User-facing (must configure)

| File | Purpose | Example |
|------|---------|---------|
| `qwen.json` | Qwen API key, ASR model, hot words | copy from `config/qwen.json.example` |
| `doubao.json` | Doubao API key, resource ID, model | copy from `config/doubao.json.example` |

### Optional (all defaults in code)

| File | Purpose |
|------|---------|
| `vinput.json` | Interaction tuning: activation delay, notifications, debounce |
| `audio.json` | Denoiser (`"none"` \| `"speexdsp"` \| `"deepfilter"`) and audio tuning |
| `zipformer.json` | Local Zipformer: model dir, binary, threads, timeout |
| `fire_red.json` | Local FireRed: model dir, binary, threads, timeout |

### API key security

API keys are plaintext fields in `qwen.json`/`doubao.json` (no secret store
integration; removed 2026-10 by design). Keep the files mode `0600`:
`chmod 600 ~/.config/vinput/{qwen,doubao}.json`.

### Config validation

Every config file is validated on read: JSON syntax errors (with byte
offset) show in the input panel and disable the file's values; unknown
fields get a "did you mean" suggestion, type mismatches and missing
required fields are reported — warnings appear in the fcitx5 log and as a
`⚠` line in the provider switch notification.

### Cloud polling

`doubao.json` tunes the async result polling. `poll_interval_msec` is the
normal polling interval (default `800`); the first query is made after 300ms
to reduce short-utterance latency, then the normal interval is restored. Keep
the default unless the API's QPS limit requires a slower cadence.

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

Capture uses an asynchronous PulseAudio stream. Releasing CapsLock returns
immediately while Vinput retains a short tail window before starting audio
processing, avoiding both input-method stalls and clipped final syllables.

## Reliability Notes

- Doubao and Qwen use a persistent serialized worker. This retains curl
  connection state across requests while preserving result order.
- Local Zipformer and FireRed runs remain one process per utterance. This is
  intentional; no long-running local model server is required.

## Developer Diagnostics

The developer build records one JSON object per line to
`~/.local/share/vinput/diagnostic.log`. It is disabled by default and can be
enabled with the Meson option below:

```bash
meson setup build-diagnostic . --wipe -Ddiagnostic_logging=true
meson compile -C build-diagnostic
sudo meson install -C build-diagnostic
fcitx5 -r
```

The optional `VINPUT_DIAGNOSTIC_LOG` environment variable overrides the log
path. The file is never rotated and stops accepting new events at 1 GiB. The
log contains no audio, API keys, or recognition text. Text and sensitive
identifiers are represented by length and truncated SHA-256 values.

After a reproduction, restart fcitx5 before collecting the file so the old
process flushes its final events:

```bash
fcitx5 -r
cp ~/.local/share/vinput/diagnostic.log /tmp/vinput-diagnostic.log
```

## License

MIT
