#!/usr/bin/env python3
"""豆包流式语音识别 WebSocket 客户端 (子进程模式)
从 stdin 读 16kHz S16LE mono PCM，结果写 stdout (JSONL)
配置: ~/.config/vinput/doubao.json {"app_key":"...","access_key":"...","resource_id":"..."}
"""
import sys, os, json, struct, asyncio, uuid
import websockets

CONFIG_PATH = os.path.expanduser("~/.config/vinput/doubao.json")
WS_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"

# WebSocket 二进制协议帧格式 (大端)
# Header: 4 bytes
#   byte0: version(4bit) | header_size(4bit)  → 0x11
#   byte1: msg_type(4bit) | flags(4bit)
#   byte2: serial(4bit) | compress(4bit)
#   byte3: reserved (0)
# Payload size: uint32 big-endian
# Payload: raw bytes

HEADER_SIZE = 0x11  # version=1, header_size=4(1x4 bytes)

def build_header(msg_type: int, flags: int = 0, serial: int = 0, compress: int = 0) -> bytes:
    return struct.pack('>BBBB', HEADER_SIZE,
                       (msg_type << 4) | flags,
                       (serial << 4) | compress,
                       0)

def build_frame(msg_type: int, payload: bytes, flags: int = 0,
                serial: int = 0, compress: int = 0) -> bytes:
    header = build_header(msg_type, flags, serial, compress)
    return header + struct.pack('>I', len(payload)) + payload

def build_full_request() -> bytes:
    """构建 Full Client Request"""
    req = {
        "audio": {
            "format": "pcm",
            "rate": 16000,
            "bits": 16,
            "channel": 1,
            "language": "zh-CN",
        },
        "request": {
            "model_name": "bigmodel",
            "enable_itn": True,
            "enable_punc": True,
        },
    }
    payload = json.dumps(req, ensure_ascii=False).encode()
    return build_frame(0x01, payload, flags=0, serial=0x01)  # type=1: full client req

def build_audio_frame(audio: bytes, is_last: bool = False) -> bytes:
    flags = 0x02 if is_last else 0x00  # 0x2 = 最后一包
    return build_frame(0x02, audio, flags=flags)  # type=2: audio only

def parse_response(frame: bytes) -> dict | None:
    """解析 Full Server Response, 提取 JSON payload"""
    if len(frame) < 4:
        return None
    # Parse header: msg_type is byte1 high 4 bits
    msg_type = (frame[1] >> 4) & 0x0F
    if msg_type == 0x0F:  # error
        offset = 4  # skip header
        err_code = struct.unpack('>I', frame[offset:offset+4])[0]
        offset += 4
        err_size = struct.unpack('>I', frame[offset:offset+4])[0]
        offset += 4
        err_msg = frame[offset:offset+err_size].decode('utf-8', errors='replace')
        print(json.dumps({"error": err_msg, "code": err_code}), flush=True)
        return None
    if msg_type != 0x09:  # 9 = full server response
        return None
    # Skip header(4) + sequence(4)
    offset = 8
    if len(frame) <= offset + 4:
        return None
    payload_size = struct.unpack('>I', frame[offset:offset+4])[0]
    offset += 4
    payload = frame[offset:offset+payload_size]
    try:
        return json.loads(payload.decode())
    except Exception:
        return None

async def main():
    # 读配置
    try:
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
    except FileNotFoundError:
        print(json.dumps({"error": f"config not found: {CONFIG_PATH}"}), flush=True)
        sys.exit(1)

    app_key = cfg.get("app_key", "")
    access_key = cfg.get("access_key", "")
    resource_id = cfg.get("resource_id", "volc.bigasr.sauc.duration")

    headers = {
        "X-Api-App-Key": app_key,
        "X-Api-Access-Key": access_key,
        "X-Api-Resource-Id": resource_id,
        "X-Api-Request-Id": str(uuid.uuid4()),
        "X-Api-Sequence": "-1",
    }

    # 按字节读 stdin: 3200 bytes = 200ms @ 16kHz S16LE mono
    CHUNK_SIZE = 3200

    loop = asyncio.get_running_loop()

    async with websockets.connect(WS_URL, extra_headers=headers, ping_interval=None) as ws:
        # 1. 发送 Full Client Request
        await ws.send(build_full_request())
        resp = await ws.recv()
        _ = parse_response(resp)  # 握手响应, 忽略

        # 2. 从 stdin 读音频并发送
        async def read_stdin():
            chunks = []
            while True:
                data = await loop.run_in_executor(None, sys.stdin.buffer.read, CHUNK_SIZE)
                if not data:
                    break
                chunks.append(data)
            return chunks

        async def send_audio(chunks: list[bytes]):
            last_idx = len(chunks) - 1
            for i, chunk in enumerate(chunks):
                is_last = (i == last_idx)
                await ws.send(build_audio_frame(chunk, is_last))
                # 收识别结果
                try:
                    resp = await asyncio.wait_for(ws.recv(), timeout=0.3)
                    result = parse_response(resp)
                    if result:
                        text = result.get("result", {}).get("text", "")
                        print(json.dumps({"text": text, "is_final": False}), flush=True)
                except asyncio.TimeoutError:
                    pass

        chunks = await read_stdin()
        await send_audio(chunks)

        # 3. 收最终结果
        try:
            while True:
                resp = await asyncio.wait_for(ws.recv(), timeout=1.0)
                result = parse_response(resp)
                if result:
                    text = result.get("result", {}).get("text", "")
                    utterances = result.get("result", {}).get("utterances", [])
                    if utterances and utterances[-1].get("definite"):
                        print(json.dumps({"text": text, "is_final": True}), flush=True)
                        break
        except asyncio.TimeoutError:
            pass


if __name__ == "__main__":
    asyncio.run(main())
