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
| `doubao.json.example` | `~/.config/vinput/doubao.json` | Doubao API key and resource ID |
| `qwen.json.example` | `~/.config/vinput/qwen.json` | Alibaba DashScope API key |
| `audio.json.example` | `~/.config/vinput/audio.json` | Active denoiser: `none`, `speexdsp`, or `deepfilter` |
| `vinput.json.example` | `~/.config/vinput/vinput.json` | Activation delay, notifications, CapsLock debounce |
| `advanced.json.example` | `~/.config/vinput/advanced.json` | Optional model paths, timeouts, thread counts, and audio tuning |

## Update Rule

When adding a new runtime config key, update the relevant `.json.example`, this README, and the user-facing configuration section in `README.md`.
