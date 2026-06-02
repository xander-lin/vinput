# Vinput 开发发现记录

## 2026-06-02

### 构建系统
- fcitx5 需要 **C++20** 编译（头文件中用了 `std::source_location`、`std::span`、`std::string_view::starts_with` 等）
- meson `default_options: ['cpp_std=c++20']`
- pkg-config 依赖名: `Fcitx5Core`

### Addon 注册机制
- `FCITX_ADDON_FACTORY(ClassName)` 宏展开为一个 `extern "C"` 函数 `fcitx_addon_factory_instance()`
- fcitx5 通过 `dlsym` 查找该符号，调用获取 Factory，再通过 `factory->create(manager)` 实例化 AddonInstance
- 定义位置: `/usr/include/Fcitx5/Core/fcitx/addoninstance.h:201`

### 安装路径
- `.so` → `/usr/lib/fcitx5/`
- `.conf` → `/usr/share/fcitx5/addon/`
- 可通过 `pkg-config --variable=libdir Fcitx5Core` 获取 libdir

### 事件监听
- `instance_->watchEvent(EventType, EventWatcherPhase, callback)` 返回 `std::unique_ptr<HandlerTableEntry<EventHandler>>`
- PreInputMethod 阶段可用于拦截快捷键（早于输入法引擎处理）
- `KeyEvent::filterAndAccept()` 拦截事件 + 标记已处理

### 跨 addon 调用 (FCITX_ADDON_DEPENDENCY_LOADER)
- 宏生成懒加载函数，首次调用时通过 `AddonManager::addon("name")` 获取目标 addon 实例
- 必须在调用它的方法之前声明（`auto` 返回类型推导依赖声明顺序）
- 必须在引用的成员变量（如 `instance_`）之后声明
- 获取实例后通过 `addon->call<INamespace::function>(args)` 调用导出函数

### 定时器
- `instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC, usec, accuracy, callback)` 创建定时器
- 返回 `std::unique_ptr<EventSourceTime>`，reset 即取消
- callback 返回 false 表示一次性定时器
- 头文件: `<fcitx-utils/event.h>`

### notifications addon
- 依赖声明: `find_package(Fcitx5Module REQUIRED)` / `dependency('Fcitx5Module')`
- addon 注册文件中声明依赖: `[Addon/Dependencies] 0=notifications`
- 跨 addon 调用 API: 头文件 `fcitx-module/notifications/notifications_public.h`
- `sendNotification(appName, replaceId, appIcon, summary, body, actions, timeout, actionCb, closedCb)`
  - 第二个参数 `replaceId` 是 `uint32_t`(数字)，不是字符串

### commitString
- `InputContext::commitString(text)` 向当前应用提交文本
- `InputContext::updateUserInterface(InputPanel)` 刷新 UI

### ASR Provider 接口
- `IAsrProvider`: 抽象基类，`start()` / `stop()` + 异步回调（onResult/onError/onState）
- `IAsrProviderFactory`: 工厂, `id()` / `name()` / `create()`
- `AsrProviderRegistry`: 全局单例注册表，adapter 通过它发现和创建后端
- MockAsrProvider: 测试用，`start()` 立即同步返回 "你好世界"
- 后续真实后端（OpenAI Whisper、Azure Speech 等）通过 `FCITX_ADDON_DEPENDENCY_LOADER` 或直接链接接入

### 常见 addon 参考
- clipboard: `fcitx5/src/modules/clipboard/` (Module 类型)
- quickphrase: `fcitx5/src/modules/quickphrase/` (PreInputMethod 阶段拦截)
- quwei: `fcitx5-quwei/` (简单 InputMethodEngine 入门示例)
