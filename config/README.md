# Configuration Examples

This directory stores example configuration files only. Packaged defaults are installed to `/etc/vinput/`; per-user overrides live in `~/.config/vinput/`.

## Boundary

- Track `*.json.example` in Git.
- Do not track `*.json` in this directory.
- Real API keys, local paths, and user preferences belong in `/etc/vinput/*.json` or `~/.config/vinput/*.json`.
- Runtime lookup order is `~/.config/vinput/*.json` first, then `/etc/vinput/*.json`; missing user files are copied from `/etc/vinput/` on first read.
- `.gitignore` ignores `config/*.json` to reduce the chance of committing real credentials.

## Initial Setup

```bash
mkdir -p ~/.config/vinput
cp config/doubao.json.example ~/.config/vinput/doubao.json
cp config/qwen.json.example ~/.config/vinput/qwen.json
cp config/audio.json.example ~/.config/vinput/audio.json
cp config/vinput.json.example ~/.config/vinput/vinput.json
cp config/advanced.json.example ~/.config/vinput/advanced.json
```

After copying, edit the files under `~/.config/vinput/`. Do not put real keys into files under this repository.

## Files

| Example | Runtime file | Purpose |
|---------|--------------|---------|
| `doubao.json.example` | `~/.config/vinput/doubao.json` | Doubao API key, resource ID, model name, ITN/punctuation |
| `qwen.json.example` | `~/.config/vinput/qwen.json` | Alibaba DashScope API key, ASR model, hot words, language hints |
| `audio.json.example` | `~/.config/vinput/audio.json` | Active denoiser: `none`, `speexdsp`, or `deepfilter` |
| `vinput.json.example` | `~/.config/vinput/vinput.json` | Activation delay, notifications, CapsLock debounce |
| `advanced.json.example` | `~/.config/vinput/advanced.json` | Optional model paths, timeouts, thread counts, and audio tuning |

## Cloud model configuration

Both cloud providers re-read their config file on every recognition, so
switching models takes effect on the next recording without restarting fcitx5.

### `qwen.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `api_key` | (required) | DashScope API key |
| `model` | `qwen-audio-3.1-asr-flash` | ASR model ID, e.g. `qwen-audio-3.0-asr-flash`, `fun-asr-flash-2026-06-15`, or legacy `qwen3-asr-flash` |
| `endpoint` | `https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation` | Full request URL; set to the `{WorkspaceId}.cn-beijing.maas.aliyuncs.com` domain if you migrate |
| `request_style` | `auto` | `auto` picks the wire format by model family; force with `input_audio` or `legacy` |
| `language_hints` | (auto-detect) | JSON array of language codes, e.g. `["zh","en"]` (max 4) |
| `keep_dialect` | `false` | `qwen-audio-3.1-asr-flash` only: keep dialect wording instead of converting to Standard Mandarin |
| `speaker_diarization` | `false` | `qwen-audio-3.1-asr-flash` only: label speakers |
| `vocabulary` | (off) | Instant hot words as `{"word": weight}`; weight 1–5, or 50 for super hot words |
| `vocabulary_id` | (off) | Precompiled hot-word list ID from the DashScope console |

### `doubao.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `api_key` | (required) | ByteDance API key |
| `resource_id` | (required) | ByteDance resource ID, e.g. `volc.seedasr.auc` |
| `model_name` | `bigmodel` | `request.model_name` sent to the submit API |
| `enable_itn` | `true` | Inverse text normalization |
| `enable_punc` | `true` | Punctuation restoration |

## Update Rule

When adding a new runtime config key, update the relevant `.json.example`, this README, and the user-facing configuration section in `README.md`.
