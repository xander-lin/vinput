# Configuration Examples

This directory stores example configuration files only. Packaged defaults are installed to `/etc/vinput/`; per-user overrides live in `~/.config/vinput/`.

## Boundary

- Track `*.json.example` in Git.
- Do not track `*.json` in this directory.
- Real API keys, local paths, and user preferences belong in `/etc/vinput/*.json` or `~/.config/vinput/*.json`.
- Runtime lookup order is `~/.config/vinput/*.json` first, then `/etc/vinput/*.json`; missing user files are copied from `/etc/vinput/` on first read.
- `.gitignore` ignores `config/*.json` to reduce the chance of committing real credentials.

## Layout: one file per concern, one file per provider

| Example | Runtime file | Purpose |
|---------|--------------|---------|
| `vinput.json.example` | `~/.config/vinput/vinput.json` | Interaction: activation delay, notifications, CapsLock debounce |
| `audio.json.example` | `~/.config/vinput/audio.json` | Denoiser choice (`none`/`speexdsp`/`deepfilter`) and audio tuning (`lufs_target`, `speex_level`, `crest_threshold`) |
| `qwen.json.example` | `~/.config/vinput/qwen.json` | Alibaba DashScope: API key, ASR model, features, timeout |
| `doubao.json.example` | `~/.config/vinput/doubao.json` | ByteDance: API key, resource ID, model, poll/timeout tuning |
| `zipformer.json.example` | `~/.config/vinput/zipformer.json` | Local Zipformer: model dir, sherpa-onnx binary, threads, timeout |
| `fire_red.json.example` | `~/.config/vinput/fire_red.json` | Local FireRed: model dir, sherpa-onnx binary, threads, timeout |

All provider configs are re-read on the worker thread before every
recognition, so model/parameter changes take effect on the next recording
without restarting fcitx5. (Breaking change 2026-10: `advanced.json` was
removed; every section moved into the file of the provider it belongs to.)

## API key lifecycle (secure + convenient)

Cloud API keys never stay in plaintext config files. Writing `api_key` into
`qwen.json`/`doubao.json` is a **one-time write channel**:

1. Put the key in the vendor JSON, e.g. `{"api_key": "sk-xxx"}`.
2. On the next recognition Vinput imports it into the encrypted Secret
   Service store (KWallet / GNOME Keyring, via `secret-tool`, secret passed
   on stdin — never on the command line, never logged).
3. The field is **removed from the JSON** by an atomic rewrite (file mode
   becomes `0600`).
4. From then on the key is read from the store. Writing `api_key` into the
   JSON again later means "update the key".

Graceful degradation: on systems without a Secret Service backend (headless
setups) the import fails, the plaintext field is **kept** (with a warning),
and the provider keeps working — nothing is lost.

Manual store operations:

```bash
# Inspect what Vinput stored
secret-tool lookup service vinput provider qwen
secret-tool lookup service vinput provider doubao

# Remove a stored key (provider then falls back to the JSON field)
secret-tool clear service vinput provider qwen
```

Requirements for the keyring path: `libsecret` (provides `secret-tool`) and
a running Secret Service (KDE Wallet or GNOME Keyring), unlocked at login —
the default on desktop sessions.

## `qwen.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `api_key` | (see lifecycle) | One-time import channel; stripped after import |
| `model` | `qwen-audio-3.1-asr-flash` | ASR model ID, e.g. `qwen-audio-3.0-asr-flash`, `fun-asr-flash-2026-06-15`, or legacy `qwen3-asr-flash` |
| `endpoint` | `https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation` | Full request URL; switch to the `{WorkspaceId}.cn-beijing.maas.aliyuncs.com` domain when migrating |
| `request_style` | `auto` | Wire format by model family; force with `input_audio` or `legacy` |
| `language_hints` | (auto-detect) | JSON array of language codes, e.g. `["zh","en"]` (max 4) |
| `keep_dialect` | `false` | `qwen-audio-3.1-asr-flash` only: keep dialect wording |
| `speaker_diarization` | `false` | `qwen-audio-3.1-asr-flash` only: label speakers |
| `vocabulary` | (off) | Instant hot words `{"word": weight}`; weight 1–5, or 50 for super hot words |
| `vocabulary_id` | (off) | Precompiled hot-word list ID from the DashScope console |
| `timeout_sec` | `60` | Whole-request curl timeout |

## `doubao.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `api_key` | (see lifecycle) | One-time import channel; stripped after import |
| `resource_id` | (required) | ByteDance resource ID, e.g. `volc.seedasr.auc` |
| `model_name` | `bigmodel` | `request.model_name` sent to the submit API |
| `enable_itn` | `true` | Inverse text normalization |
| `enable_punc` | `true` | Punctuation restoration |
| `poll_interval_msec` | `800` | Query poll interval |
| `max_polls` | `75` | Give up after this many polls |
| `submit_timeout_sec` | `30` | Submit request timeout |
| `query_timeout_sec` | `15` | Each query request timeout |

## Initial Setup

```bash
mkdir -p ~/.config/vinput
cp config/vinput.json.example ~/.config/vinput/vinput.json
cp config/audio.json.example ~/.config/vinput/audio.json
cp config/qwen.json.example ~/.config/vinput/qwen.json      # then add api_key once
cp config/doubao.json.example ~/.config/vinput/doubao.json  # then add api_key once
```

After copying, edit the files under `~/.config/vinput/`. Do not put real keys into files under this repository.

## Update Rule

When adding a new runtime config key, update the relevant `.json.example`, this README, and the user-facing configuration section in `README.md`.
