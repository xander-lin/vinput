# Tests

This directory contains automated tests that run through Meson with no microphone, desktop session, local ASR model, or cloud API key.

## Test Boundary

- `test_asr_registry.cpp` verifies the ASR provider registry contract and the built-in `mock` provider.
- `test_audio_capture_file.cpp` verifies the file-only audio processing path by generating a deterministic 16 kHz mono sample buffer, running the `none` denoiser path, and checking the WAV written by `AudioCapture`.
- `test_buffer_detect_cache.cpp` verifies that the hardware buffer cache migrates the legacy single `buffer_bytes` field to the current PulseAudio source and keeps separate cached values for separate source ids.
- Hardware and desktop integration checks stay in `tools/` because they depend on PulseAudio devices, fcitx5, and a running compositor.

## Fixture Policy

Current automated tests generate their own deterministic audio fixture in memory. Add committed fixtures under `tests/fixtures/` only when a regression requires real recorded audio that cannot be represented by generated samples. Document the source, sample rate, channel count, covered scenario, and update procedure next to the fixture.

## Run

```bash
meson test -C build
```
