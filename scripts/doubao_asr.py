#!/usr/bin/env python3
"""豆包流式语音识别 WebSocket 客户端 (子进程模式)
从 stdin 读 16kHz S16LE mono PCM，结果写 stdout (JSONL)
配置: ~/.config/vinput/doubao.json {"api_key":"...","resource_id":"..."}
"""
import sys, os, json, struct, asyncio, uuid
import websockets

CONFIG_PATH = os.path.expanduser("~/.config/vinput/doubao.json")
WS_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"

# 200ms @ 16kHz S16LE mono = 3200 samples = 6400 bytes
CHUNK_BYTES = 6400

# --- 二进制帧协议 ---
def build_header(msg_type: int, flags: int = 0,
                 serial: int = 0, compress: int = 0) -> bytes:
    """4-byte header: ver(4)|hdr_sz(4)  type(4)|flags(4)  ser(4)|cmp(4)  reserved(8)"""
    return struct.pack('>BBBB',
                       0x11,  # version=1, header_size=4
                       (msg_type << 4) | flags,
                       (serial << 4) | compress,
                       0)

def build_frame(msg_type: int, payload: bytes, flags: int = 0) -> bytes:
    hdr = build_header(msg_type, flags)
    return hdr + struct.pack('>I', len(payload)) + payload

def build_full_request() -> bytes:
    req = {
        "audio": {"format": "pcm", "rate": 16000, "bits": 16, "channel": 1, "language": "zh-CN"},
        "request": {"model_name": "bigmodel", "enable_itn": True, "enable_punc": True},
    }
    return build_frame(0x01, json.dumps(req, ensure_ascii=False).encode())

def build_audio_frame(audio: bytes, is_last: bool = False) -> bytes:
    return build_frame(0x02, audio, flags=0x02 if is_last else 0x00)  # type=2: audio only

def parse_response(data: bytes) -> dict | None:
    """解析 full server response 帧, 返回 JSON dict 或 None"""
    if len(data) < 8:
        return None
    msg_type = (data[1] >> 4) & 0x0F
    if msg_type == 0x0F:  # error
        off = 4
        err_code = struct.unpack('>I', data[off:off+4])[0]; off += 4
        err_size = struct.unpack('>I', data[off:off+4])[0]; off += 4
        err_msg = data[off:off+err_size].decode(errors='replace')
        print(json.dumps({"error": err_msg, "code": err_code}), flush=True)
        return None
    if msg_type != 0x09:  # 9 = full server response
        return None
    off = 8  # skip header(4) + sequence(4)
    if len(data) <= off + 4:
        return None
    sz = struct.unpack('>I', data[off:off+4])[0]; off += 4
    try:
        return json.loads(data[off:off+sz].decode())
    except Exception:
        return None

# --- 主流程 ---
async def main():
    try:
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
    except FileNotFoundError:
        print(json.dumps({"error": f"config not found: {CONFIG_PATH}"}), flush=True)
        sys.exit(1)

    api_key = cfg.get("api_key", cfg.get("app_key", ""))
    resource_id = cfg.get("resource_id", "volc.bigasr.sauc.duration")

    headers = {
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": resource_id,
        "X-Api-Request-Id": str(uuid.uuid4()),
        "X-Api-Sequence": "-1",
    }

    loop = asyncio.get_running_loop()

    try:
        async with websockets.connect(WS_URL, extra_headers=headers, ping_interval=None,
                                      open_timeout=10) as ws:
            # 1. Full Client Request
            await ws.send(build_full_request())
            _ = await ws.recv()  # 握手响应, 忽略

            # 2. 边读 stdin 边发送
            async def recv_results():
                final = False
                while not final:
                    try:
                        data = await asyncio.wait_for(ws.recv(), timeout=0.5)
                        r = parse_response(data)
                        if r and "result" in r:
                            txt = r["result"].get("text", "")
                            utts = r["result"].get("utterances", [])
                            definite = utts and utts[-1].get("definite", False) if utts else False
                            if txt:
                                print(json.dumps({"text": txt, "is_final": definite},
                                                 ensure_ascii=False), flush=True)
                                if definite:
                                    final = True
                    except asyncio.TimeoutError:
                        pass
                    except websockets.ConnectionClosed:
                        break

            recv_task = asyncio.create_task(recv_results())

            # 3. 读 stdin 分块发送
            while True:
                data = await loop.run_in_executor(None, sys.stdin.buffer.read, CHUNK_BYTES)
                if not data or len(data) < CHUNK_BYTES:
                    if data:
                        await ws.send(build_audio_frame(data, is_last=True))
                    else:
                        await ws.send(build_audio_frame(b"", is_last=True))
                    break
                await ws.send(build_audio_frame(data))

            # 4. 等最终结果
            await recv_task

    except Exception as e:
        print(json.dumps({"error": f"Connection failure: {e}"}), flush=True)

if __name__ == "__main__":
    asyncio.run(main())
