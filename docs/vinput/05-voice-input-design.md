# Vinput 语音输入插件设计参考

## 架构设计

```
┌─────────────────────────────────────────┐
│              fcitx5 Server               │
│                                          │
│  ┌────────────────────────────────────┐  │
│  │     adapter/ (Module Addon)        │  │
│  │  - 注册到 fcitx5 框架              │  │
│  │  - 监听触发键                       │  │
│  │  - 调用 ASR 后端                    │  │
│  │  - 向 InputContext 提交文本         │  │
│  │  - 显示语音状态 (preedit/aux)      │  │
│  └────────────┬───────────────────────┘  │
│               │                          │
└───────────────┼──────────────────────────┘
                │
  ┌─────────────┴──────────────────────────┐
  │         ASR_provider/                   │
  │  - 音频采集 (PulseAudio/PipeWire/ALSA)  │
  │  - 音频流式传输                         │
  │  - ASR 后端接口 (OpenAI, Azure, ...    │
  │  - 本地引擎 (Whisper, Vosk, ...)       │
  └─────────────────────────────────────────┘
```

## Adapter 模块设计要点

### 1. Addon 类型

使用 **Module** 类型 (Category=Module)，不定义 InputMethodEngine。

### 2. 事件处理

在 **PreInputMethod** 阶段监听键事件：
- 拦截触发键 (如 Ctrl+Alt+V)
- 切换语音输入开/关状态
- 在语音输入激活期间，可选择拦截或透传其他键

### 3. 文本提交

通过 InputContext 的 `commitString()` 方法提交识别结果：

```cpp
ic->commitString(recognizedText);
ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
```

### 4. UI 反馈

使用 InputPanel 提供可视化反馈：

```cpp
auto &inputPanel = ic->inputPanel();
// 语音输入状态提示
inputPanel.setAuxUp(fcitx::Text("🎤 Listening..."));
// 或使用 client preedit 显示部分识别结果
inputPanel.setClientPreedit(fcitx::Text(partialText));
```

注意：语音输入 addon 不应设置自己的 UI addon，应使用 Fcitx 内建 UI。

### 5. 状态管理

使用 InputContextProperty 为每个输入上下文维护独立状态：

```cpp
struct VoiceInputState {
    bool recording = false;
    std::string partialResult;
    std::string finalResult;
    // ASR session reference
};
```

## 关键 API 用到的头文件

```cpp
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/configuration.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
```

## 编译依赖

```cmake
find_package(Fcitx5Core REQUIRED)
target_link_libraries(vinput PRIVATE Fcitx5::Core)
```

## 已有参考

目前 fcitx5 生态中尚无官方的语音输入 addon。以下可以参考的类似实现：

- **fcitx5-module-cloudpinyin** (云拼音): 展示了如何调用外部 API 获取 text -> 候选词的转换
- **fcitx5-module-clipboard**: 展示 Module 类型 addon 的完整实现
- **fcitx5-module-quickphrase**: 展示 PreInputMethod 阶段事件处理 + InputContextProperty 使用
