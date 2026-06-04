#!/usr/bin/env python3
"""Test the sherpa-onnx-offline-websocket-server directly."""
import struct, asyncio, sys
from pathlib import Path

import websockets

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18000
WAV = sys.argv[2] if len(sys.argv) > 2 else None

async def test():
    if not WAV:
        # Generate a test tone: 1s of 440Hz sine at 16kHz
        import math
        sr = 16000
        duration = 2.0
        nsamples = int(sr * duration)
        samples_i16 = [int(16000 * math.sin(2 * math.pi * 440 * i / sr))
                       for i in range(nsamples)]
        wav_bytes = build_wav(samples_i16)
    else:
        wav_bytes = Path(WAV).read_bytes()

    # Parse WAV header
    assert wav_bytes[:4] == b'RIFF'
    sample_rate = struct.unpack_from('<i', wav_bytes, 24)[0]
    pcm_i16 = wav_bytes[44:]

    # Convert int16 -> float32
    floats = struct.unpack('<' + 'h' * (len(pcm_i16) // 2), pcm_i16)
    floats = [f / 32768.0 for f in floats]

    # Build protocol payload
    payload = struct.pack('<i', sample_rate)  # int32_le sample_rate
    payload += struct.pack('<i', len(floats) * 4)  # int32_le float_byte_count
    payload += struct.pack('<' + 'f' * len(floats), *floats)

    print(f"Connecting to ws://127.0.0.1:{PORT} ...")
    print(f"  sample_rate={sample_rate}, num_samples={len(floats)}, payload_bytes={len(payload)}")

    async with websockets.connect(f"ws://127.0.0.1:{PORT}") as ws:
        await ws.send(payload)
        print(f"  sent {len(payload)} bytes, waiting for response...")
        response = await ws.recv()
        print(f"  result ({len(response)} bytes): {response}")
        return response

def build_wav(samples_i16):
    """Build minimal WAV file in memory."""
    data_size = len(samples_i16) * 2
    header = struct.pack(
        '<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + data_size, b'WAVE',
        b'fmt ', 16, 1, 1, 16000, 32000, 2, 16,
        b'data', data_size)
    return header + struct.pack('<' + 'h' * len(samples_i16), *samples_i16)

if __name__ == '__main__':
    r = asyncio.run(test())
