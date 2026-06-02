# FireRedASR2-LLM 后端资料

## 基本信息

| 项目 | 值 |
|------|-----|
| 来源 | 小红书 FireRed 团队 |
| 模型 | FireRedASR2-LLM (8B+ 参数) |
| 架构 | Encoder-Adapter-LLM (基于 Qwen2-7B) |
| 许可证 | Apache 2.0 |
| 代码 | https://github.com/FireRedTeam/FireRedASR2S |
| 论文 | https://arxiv.org/abs/2603.10420 |
| 模型下载 | https://huggingface.co/FireRedTeam/FireRedASR2-LLM |
| 镜像下载 | https://www.modelscope.cn/models/xukaituo/FireRedASR2-LLM/ |

## 指标 (CER%, 越低越好)

| 场景 | FireRedASR2-LLM | Doubao-ASR | Qwen3-ASR |
|------|:-:|:-:|:-:|
| 普通话 (4 测试集均值) | **2.89** | 3.69 | 3.76 |
| 方言 (19 测试集均值) | **11.55** | 15.39 | 11.85 |

## 限制

- 音频格式: 16kHz 16-bit mono PCM wav
- 最大输入: **40 秒**
- 必须 GPU (8B+ 参数)
- 需要 Python 3.10

## Python API

```python
from fireredasr2s.fireredasr2 import FireRedAsr2, FireRedAsr2Config

config = FireRedAsr2Config(
    use_gpu=True,
    repetition_penalty=1.0,
    llm_length_penalty=0.0,
    temperature=1.0
)
model = FireRedAsr2.from_pretrained("llm", "pretrained_models/FireRedASR2-LLM", config)
results = model.transcribe(["uttid1"], ["/tmp/audio.wav"])
print(results[0]["text"])  # "你好世界"
```

## 安装步骤

```bash
git clone https://github.com/FireRedTeam/FireRedASR2S.git
conda create -n fireredasr2s python=3.10
conda activate fireredasr2s
cd FireRedASR2S
pip install -r requirements.txt
export PYTHONPATH=$PWD/:$PYTHONPATH

# 下载模型 (ModelScope 国内更快)
modelscope download --model xukaituo/FireRedASR2-LLM --local_dir ./pretrained_models/FireRedASR2-LLM
```

## Vinput 集成方案

C++ adapter 无法直接调用 Python 模型，采用**子进程方案**：

```
adapter (C++)                           Python 子进程
    │                                       │
    ├─ 录音 → /tmp/vinput_audio.wav          │
    ├─ spawn python3 transcribe.py ─────────>│
    │                                       ├─ 加载模型 (启动时预热)
    │                                       ├─ 读 wav
    │                                       ├─ model.transcribe()
    │                                       ├─ stdout → JSON
    │─ stdout ← JSON ───────────────────────┤
    ├─ commitString(text)
```

- 录音: 使用 PipeWire/PulseAudio API 或直接调用 `parec` 等工具
- 子进程: stdin/stdout 通信，避免每次启动加载模型的开销（常驻进程）
- Python 脚本常驻运行，启动时加载一次模型
