# Vinput Voice Input Design

## Current Architecture

```text
fcitx5
  |
  v
adapter/VinputAddon
  |-- key lifecycle: CapsLock press/release and switch mode
  |-- provider/denoiser selection and notifications
  |-- AudioCapture lifecycle
  |-- IAsrProvider lifecycle
  `-- OutputHandler lifecycle

ASR_provider/AudioCapture
  |-- PulseAudio capture
  |-- hardware buffer detection
  |-- ebur128 normalization
  |-- denoise and VAD trim
  `-- samples + wavPath

ASR_provider/IAsrProvider
  |-- transcribe(samples, wavPath)
  `-- onResult(text, isFinal) / onError(error)

adapter/OutputHandler
  |-- self-pipe thread handoff
  |-- DesktopStrategy focus switch when supported
  `-- fcitx InputContext::commitString(text)
```

## Module Boundaries

`adapter/` owns fcitx5 integration. It listens to keyboard events, starts and stops recording, chooses ASR providers and denoisers, shows notifications, and commits text through fcitx5.

`ASR_provider/` owns audio capture, audio preprocessing, provider abstraction, provider registration, and ASR protocol implementations. It does not know about fcitx5 input contexts or desktop focus.

`OutputHandler` owns the ASR result to text commit boundary. It accepts text from worker threads, returns to the fcitx event loop through a self-pipe, optionally switches focus through `DesktopStrategy`, and commits to the currently usable input context.

`DesktopStrategy` owns compositor-specific focus commands. Unsupported desktops use `NoopStrategy`, which disables focus switching and falls back to direct commit.

## Input Flow

```text
CapsLock press
  -> VinputAddon stores timing and input context metadata
  -> after activation delay, VinputAddon creates provider and AudioCapture
  -> OutputHandler captures focused desktop window
  -> AudioCapture records and preprocesses audio

CapsLock release
  -> AudioCapture::stop() joins the capture thread
  -> recorded callback calls IAsrProvider::transcribe(samples, wavPath)
  -> provider calls onResult/onError
  -> OutputHandler::submit(text)
  -> commitString(text)
  -> VinputAddon restores CapsLock state through uinput
```

## Switch Flow

`Ctrl+CapsLock` enters switch mode without starting audio capture. Horizontal keys switch ASR providers. Vertical keys switch denoisers. Selections are persisted to the existing fcitx config or `~/.config/vinput/audio.json`.

## Failure Modes

| Area | Failure | Handling |
|------|---------|----------|
| AudioCapture | PulseAudio open/read failure | error/status logging; no provider call if no samples |
| AudioCapture | missing deepfilter binary | denoiser path reports failure and leaves capture flow controlled by `AudioCapture` |
| Provider | missing API key or model binary | provider calls `onError` or logs provider-specific failure |
| OutputHandler | unsupported compositor | `NoopStrategy` disables focus switching and commits directly |
| OutputHandler | focus switch timeout | commits anyway and attempts focus restore |
| uinput | `/dev/uinput` unavailable | logs failure; voice input can still work but CapsLock restore is unavailable |

## Verification

Automated tests cover contracts that do not require desktop, microphone, model files, or cloud keys:

```bash
meson test -C build
```

Manual tools in `tools/` cover hardware and audio integration:

```bash
./build/tools/test_audio_file input.wav output.wav none
./build/tools/test_audio_pipeline output.wav 3 none
```

Full integration requires installing the addon and restarting fcitx5:

```bash
sudo meson install -C build
fcitx5 -r
```
