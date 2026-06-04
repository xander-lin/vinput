# 千问3-ASR-Flash API

千问3-ASR-Flash 通过 DashScope multimodal 接口调用，支持 base64 内联音频数据。

## 与 Filetrans 的区别

| 特性 | qwen3-asr-flash | qwen3-asr-flash-filetrans |
|------|----------------|--------------------------|
| 音频长度 | ≤5 分钟 | ≤12 小时 |
| 文件大小 | ≤10MB | ≤2GB |
| 输入方式 | URL / base64 Data URL / 本地路径 | 仅公网 URL |
| 调用模式 | 同步（单次 HTTP） | 异步（submit + poll） |
| 流式输出 | 支持 | 不支持 |

语音输入场景选用 **qwen3-asr-flash**，因为支持 base64 内联音频，无需公网 URL。

## API 详情

### 提交转写（同步调用）

**端点**: `POST https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation`

**Header**:

| Key | Value |
|-----|-------|
| Authorization | Bearer {API_KEY} |
| Content-Type | application/json |

**Body**:

```json
{
    "model": "qwen3-asr-flash",
    "input": {
        "messages": [
            {
                "content": [
                    {"audio": "data:audio/wav;base64,{BASE64_DATA}"}
                ],
                "role": "user"
            }
        ]
    },
    "parameters": {
        "asr_options": {
            "enable_itn": false
        }
    }
}
```

### 响应

成功 (HTTP 200):

```json
{
    "output": {
        "choices": [{
            "message": {
                "content": [{"text": "识别出的文本内容"}]
            }
        }]
    }
}
```

文本提取路径: `output.choices[0].message.content[0].text`

### 配置

文件: `~/.config/vinput/qwen.json`

```json
{"api_key": "sk-xxx"}
```

API Key 获取: https://bailian.console.aliyun.com/?tab=model#/api-key

### 流式输出

设置 header `X-DashScope-SSE: enable` 可在响应中获取 SSE 事件流。当前 provider 实现使用非流式调用，一次返回完整结果。流式可用作后续优化。

### 限制

- 100 QPS (异步模式) / 本地文件上传 100 QPS
- 音频限制: ≤10MB, ≤5 分钟
- 支持格式: wav, mp3 等主流格式
- Base64 编码会增大体积 ~33%，需确保编码后 ≤10MB

### 地域

- 中国内地（北京）: `dashscope.aliyuncs.com`
- 新加坡: `dashscope-intl.aliyuncs.com`
- 美国: 使用 `qwen3-asr-flash-us` 模型
