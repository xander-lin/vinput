# Addon 系统详解

## Addon 注册文件

每个 addon 需要一个 `.conf` 配置文件注册，文件名即为 addon 的唯一标识。

### 基本格式

```ini
[Addon]
Name=MyAddon
Category=Module
Version=1.0.0
Library=myaddon
Type=SharedLibrary
OnDemand=True
Configurable=True

[Addon/Dependencies]
0=punctuation

[Addon/OptionalDependencies]
0=fullwidth
1=quickphrase
```

### 字段说明

| 字段         | 说明                                                         |
| ------------ | ------------------------------------------------------------ |
| Name         | 可翻译的 addon 名称                                          |
| Category     | addon 分类: InputMethod / Module / Frontend / UI             |
| Version      | 版本号                                                       |
| Library      | 共享库文件名 (不含 lib 前缀和 .so 后缀)                      |
| Type         | SharedLibrary (标准) 或 StaticLibrary                        |
| OnDemand     | 是否按需加载 (True 表示有输入法选中时才加载)                 |
| Configurable | 是否可配置 (True 时 Configtool 会生成配置 UI)                |
| Dependencies | 强制依赖的 addon 列表                                        |
| OptionalDependencies | 可选依赖 addon                                               |

### C++ 端

```cpp
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>

class MyAddon : public fcitx::AddonInstance {
    // ...
};

class MyAddonFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new MyAddon;
    }
};

// 注册 factory
FCITX_ADDON_FACTORY(MyAddonFactory);
```

## 事件处理管道

Fcitx 使用事件管道处理键事件，分为 5 个阶段（按优先级排序）：

| 阶段            | 可见性     | 用途                       |
| --------------- | ---------- | -------------------------- |
| ReservedFirst   | 内部       | Fcitx 内部保留             |
| PreInputMethod  | 公开       | **子输入模式/快捷键拦截** (最常用) |
| Default         | 公开       | 活跃输入法引擎处理         |
| PostInputMethod | 公开       | 输入法处理后               |
| ReservedLast    | 内部       | Fcitx 内部保留             |

### PreInputMethod 的典型用法

一个 addon 可以定义触发键，按下后进入子输入模式，在 PreInputMethod 阶段拦截所有后续键事件：

```cpp
// 在 addon 构造函数中注册事件处理
eventHandler_ = instance_->watchEvent(
    EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
    [this](Event &event) {
        auto &keyEvent = static_cast<KeyEvent &>(event);
        if (keyEvent.isRelease()) return;
        if (keyEvent.key().checkKey(triggerKey_)) {
            activateVoiceInput(keyEvent.inputContext());
            return keyEvent.filterAndAccept();
        }
    });
```

## InputContext (输入上下文)

InputContext 代表 fcitx 服务器的一个客户端。通常映射到一个应用、一个窗口、或是显示服务器的全局上下文。

- 每个 InputContext 可以有自己的状态 (通过 InputContextProperty)
- InputContext 按 display 连接分组到 FocusGroup
- 每个 FocusGroup 中最多只有一个 InputContext 获得焦点

### InputContextProperty

用于为每个 InputContext 存储独立状态：

```cpp
// 注册 property factory
auto &factory = instance_->inputContextManager().registerProperty(
    "myAddonState",
    FactoryFor<MyState>(
        [this](InputContext &ic) { return new MyState(ic); }
    )
);

// 获取 state
auto *state = ic->propertyFor(&factory_);
```

## 关键 API 类

| 类名                          | 作用                               |
| ----------------------------- | ---------------------------------- |
| `AddonInstance`               | 所有 addon 的基类                  |
| `AddonFactory`                | Addon 实例化工厂                   |
| `AddonManager`                | Addon 管理器，可加载/卸载 addon    |
| `InputMethodEngineV2`         | 输入法引擎 addon 基类              |
| `InputContextManager`         | 管理所有 InputContext              |
| `InputContext`                | 单个输入上下文                     |
| `InputPanel`                  | 输入面板 (preedit + candidate list)|
| `KeyEvent`                    | 键事件                             |
| `Key`                         | 键对象 (含规范化 key)              |
| `Configuration`               | 配置基类                           |

## 代码文档链接

- AddonInstance: https://codedocs.xyz/fcitx/fcitx5/classfcitx_1_1AddonInstance.html
- AddonFactory: https://codedocs.xyz/fcitx/fcitx5/classfcitx_1_1AddonFactory.html
- InputMethodEngineV2: https://codedocs.xyz/fcitx/fcitx5/classfcitx_1_1InputMethodEngineV2.html
- InputContext: https://codedocs.xyz/fcitx/fcitx5/classfcitx_1_1InputContext.html
- KeyEvent: https://codedocs.xyz/fcitx/fcitx5/classfcitx_1_1KeyEvent.html
- InputPanel: https://codedocs.xyz/fcitx/fcitx5/classfcitx_1_1InputPanel.html
