# fcitx5-vinput — Project AGENTS.md

## Project Identity And Purpose

fcitx5-vinput is a voice input addon for fcitx5 (Linux IME framework). The user
holds **CapsLock**, speaks, releases, and the recorded audio is transcribed by an
ASR backend and committed as text into the focused application. It serves users
who want fast push-to-talk dictation without leaving the keyboard.

## Functional Goals And Scope

- Push-to-talk capture (CapsLock hold), status UI via fcitx5 input panel,
  consecutive-speech queueing (max 3 in-flight recognitions, FIFO order
  preserved per app/input context).
- Pluggable ASR backends behind `IAsrProvider` / `IAsrProviderFactory` and the
  `AsrProviderRegistry`:
  - `mock` — deterministic test provider.
  - `zipformer`, `fire_red` — local providers via external `sherpa-onnx` binaries.
  - `doubao` (ByteDance), `qwen` (Alibaba DashScope) — cloud providers via libcurl,
    persistent serialized worker threads.
- Audio capture with optional denoising; cloud failures surfaced as actionable
  status errors (network / service / timeout / microphone / no-speech).
- Scope boundary: short-duration dictation only (≤ minutes). Long-audio
  file transcription (filetrans/async job APIs) is out of scope.

## Product Philosophy

- Low latency and predictable UX first; diagnostics and timers are built in
  (`Vinput Qwen [timer] encode/network/parse` logs, `diagnostic_log` events).
- Every provider must degrade loudly and recoverably: cancellable requests,
  curl handle eviction on transport failure, actionable error strings.
- Configuration is one plain-JSON file per concern/provider (`qwen.json`,
  `doubao.json`, `audio.json`, `vinput.json`, `zipformer.json`,
  `fire_red.json`), examples tracked in `config/`,
  installed to `/etc/vinput/`, overridable per-user in `~/.config/vinput/`.
  `advanced.json` was removed in the 2026-10 breaking restructure — each
  provider owns exactly one file now.
- Cloud API keys follow the keyring lifecycle in `secret_store.h`: the
  `api_key` JSON field is a one-time import channel into the Secret Service
  store and is stripped after a successful import; without a keyring backend
  the plaintext field is kept.

## Project Principles

- Modular, pluggable providers: a new ASR backend = new `{id}_provider.{h,cpp}`
  in `ASR_provider/src/` + factory self-registration + one line in
  `adapter/src/vinput.cpp` to link. Keep mock provider as the interface spec.
- Cloud providers own a single serialized worker thread; tasks are queued;
  cancellation is cooperative via `std::atomic_bool`.
- JSON parsing in providers is intentionally dependency-free (string find /
  manual extraction). Keep it minimal but robust to field order.
- Build: meson (`meson.build`, `meson_options.txt`, per-dir `meson.build`).
  Packaging: PKGBUILD (Arch). Verification: `ninja -C build` + tests under
  `tests/`.
- Docs live in `docs/` (`qwen-asr-api.md` etc.), accumulated engineering notes
  in `FINDINGS.md` (historical, at repo root).

## Current Cloud Model Status (verified 2026-10)

- `qwen` provider is config-driven end to end (`~/.config/vinput/qwen.json`):
  model (default `qwen-audio-3.1-asr-flash`), endpoint, request style
  (auto-detected per model family), language hints, instant/precompiled hot
  words, dialect retention, speaker diarization. Config is re-read on every
  recognition, so model switches need no restart or rebuild. Legacy
  `qwen3-asr-flash` still works by setting `model` (old wire format is kept).
- `doubao` provider likewise takes `model_name` / `enable_itn` / `enable_punc`
  from `doubao.json`.
- Cross-vendor wire unification was evaluated and rejected: Doubao uses a
  submit/query poll protocol with `X-Api-Key` headers and header-based status
  codes, Qwen uses a synchronous Bearer-auth JSON call. Vendors stay one
  provider each behind `IAsrProvider`; only the config schema is unified.
- JSON parsing helpers live in `src/qwen_json.h` and are unit-tested in
  `tests/test_qwen_json.cpp`. When Alibaba deprecates the next model,
  the expected fix is a config/example/doc update, not code — unless the wire
  protocol itself changes.
