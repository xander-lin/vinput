# Vinput 开发发现记录

## 2026-06-03

### CX31993 硬件占空比（录音尾部丢失根因）

#### 发现过程
- `tools/record_timing.cpp` 测试发现无论 buffer 大小（128/1024/4096），`pa_simple_read` 始终呈现 **~2s 阻塞 + ~2s burst（0ms reads）** 的 50% 占空比
- 48kHz 原生采样率也如此，parecord 同效
- 即使在录音同时另开一条常驻流读 48kHz，占空比不变
- **结论**: CX31993 芯片硬件内部以 ~2s 为周期填充缓冲 → PulseAudio 瞬间取走（burst）→ 下一轮填充（阻塞 2s），任何持续读取都无法打破

#### 后果
- 4s 录音: 前 2s burst 被捕获，后 2s 落在下一轮填充期（丢失）
- 这就是「后半段丢失」的根因

#### 解决方案
- **Drain**: `stop()` 后不立即停止录音，继续读直到检测到 2 次长阻塞（＞500ms）或 4s 超时
  - 这确保了跨过一个完整 burst 周期，捕获尾部音频
  - 代价: 松键后 1-4s 延迟出结果
- **去掉 keep-alive**: keep-alive 流不能阻止 CX31993 的硬件占空比，是之前的误判

#### 实测数据
```
4096 buf: 63 次, eff=80%, max=2036ms  ┐
1024 buf: 251 次, eff=80%, max=2005ms ─ 完全一致
128  buf: 2001 次, eff=80%, max=2005ms ┘
```

### 豆包录音文件识别 API（非流式）

#### API 流程
- **提交任务**: `POST /api/v3/auc/bigmodel/submit` → 返回 `X-Api-Status-Code: 20000000`
- **轮询结果**: `POST /api/v3/auc/bigmodel/query` → 直到 `20000000`（成功）、`20000003`（静音）或超时
- **比流式更适合语音输入**: 先录完再识别，结果一次返回，不需要处理 preedit 中间态

#### audio.data 字段（内联音频）
- 官方文档只有 `audio.url`，但 **`audio.data`（base64 编码）实测可用**
- `format:"wav"` + `data:"<base64>"` 提交成功，服务端正常解码
- 注意：`format:"raw"` 会返回 `45000151`（音频格式不正确），服务端无法解析裸 PCM
- base64 大小：4 秒 16kHz mono WAV ≈ 174KB，在 curl 的默认限制内

#### Header 解析陷阱
- 服务端返回的 header 名是**小写** `x-api-status-code`（不是 `X-Api-Status-Code`）
- `access-control-expose-headers` 头里**恰好包含字串** `X-Api-Status-Code`
- 用 `find("X-Api-Status-Code")` 会命中 `access-control-expose-headers` 而非真正的 status header
- **修复**: 匹配 `\n<name>:` 模式，确保匹配的是独立的 header 行而非其他 header 的值

#### 错误码
| 错误码 | 含义 | 处理 |
|--------|------|------|
| 20000000 | 成功 | 提取 `result.text` |
| 20000001/2 | 处理中 | 继续轮询 |
| 20000003 | 静音音频 | 返回空文本，不报错 |
| 45000151 | 音频格式不正确 | format/编码不匹配 |
| 45000001 | 参数无效 | 检查必填字段 |

#### 轮询策略
- 间隔 800ms，最长 60 秒（75 次）
- 每次 `curl_easy_init/cleanup` 新建连接，避免复用导致的状态问题
- HTTP 层超时设为 15s，curl 错误时 continue 重试

#### 配置
- 文件: `~/.config/vinput/doubao.json`
- 格式: `{"api_key":"xxx", "resource_id":"volc.seedasr.auc"}`
- 资源 ID: `volc.bigasr.auc` (1.0 小时版) / `volc.seedasr.auc` (2.0)

#### 实现注意事项
- **不需要 keep-alive 流**也可以（zipformer 的 keep-alive 是为了防 PipeWire 挂起），共用 zipformer 的常驻流模式
- `curl_global_init(CURL_GLOBAL_ALL)` 必须在任何 curl 操作前调用（静态初始化器）
- UUID 用 `getrandom()` 而非 `rand()`，避免冲突和时间种子质量差
- 响度归一化（ebur128 → -16 LUFS）对云 ASR 同样有效，避免音量过低导致 20000003

### PipeWire/PA 录音（keep-alive 被证实为误判，见上方 CX31993 条目）
- ~~使用 pa_simple_read 录音效率仅 67%~~ → 实际是 CX31993 硬件占空比
- ~~PipeWire suspend-node 导致设备挂起~~ → 测试证明加常驻流也无法阻止
- ~~常驻录音流解决~~ → keep-alive 流已移除，替换为按需录音 + drain

### Zipformer 调试

#### sherpa-onnx 输出流
- sherpa-onnx 将文本结果和 JSON 输出到 **stderr**，而非 stdout
- stdout 只有 "Start to create recognizer" 和 "Recognizer created in X s"
- 用 `popen("r")` 只能读 stdout，需改为 fork + pipe 分开捕获 stdout/stderr
- **选项格式**: sherpa-onnx 要求 `--encoder=value`（带`=`），用 `execl` 时必须拼接为单个 argv 字符串，不能用空格分隔的独立参数

#### 录音竞争条件
- `stop()` 中用 `std::thread(...).detach()` 启动子进程识别
- 析构函数中 `unlink(tempWavPath_)` 删除 WAV，与 detached 线程存在 **TOCTOU 竞争**
- 修复: `unlink(wav)` 移到 detached 线程中 `pclose` 之后执行，析构函数不删文件
- 线程安全: lambda 按值捕获 `modelDir_`，不持有 `this` 指针

#### 退出码 65280
- 65280 = 0xFF00 = `WEXITSTATUS(255)` × 256
- `pclose` 返回 raw wait status，需用 `WIFEXITED/WEXITSTATUS` 提取退出码
- exit code 255 常见原因: shell 找不到命令 / 文件不存在 / 参数格式错误

#### JSON 解析
- `rfind("\"text\"")` 定位到 `"text":` key 后，需跳过 9 个字符（不是 8 个）
  - `"text"` = 6, `": "` = 2, 值开头的 `"` = 1, 共 9
- `pos += 8` 正好落在值开头的引号上，`find('"', pos)` 找到同一字符，substr 结果为空

#### 响度归一化
- 简单峰值归一化不如 EBU R128 标准（K-weighting + gating）
- 使用 **libebur128** (`pkg-config: libebur128`)，依赖加入 `adapter/src/meson.build`
- ebur128 API: `ebur128_init(ch, sr, EBUR128_MODE_I)` → `ebur128_add_frames_short()` → `ebur128_loudness_global()` → `ebur128_destroy()`
- 归一化目标: **-16 LUFS**（语音播客标准响度，比广播 -23 LUFS 更适合 ASR）
- **边界处理**: `loudness < -70` 或 `-inf` 时 skip 归一化（gain=1.0）
- gain 裁剪: 0.05x ~ 20x，防止极端情况

#### 录音流程改进
- 改为先收集全部 PCM 到 `std::vector<int16_t>`，归一化后再写 WAV，便于内存中处理
- 添加 `max_abs` 调试日志，方便排查录音是否静音
- `pa_simple_read` 阻塞直到 `bytes` 字节就绪：`sizeof(buf)=4096` 在 16kHz S16LE 下 ≈128ms/iter

#### 音量对识别的影响（sherpa-onnx zipformer）
- 用测试音频 (10s) 降幅测试:
  - 100% → 完整识别全文
  - 10% → 部分缺漏
  - 1% → 几乎全丢 (只识别尾部少数词)
- `normalize_samples=True` 有内部归一化，但极端小振幅 + 量化噪声时无效

### 构建依赖
- libebur128: Arch `extra/libebur128`, 头文件 `<ebur128.h>`, meson `dependency('libebur128')`
- parecord 测试麦克风: `parecord --rate=16000 --format=s16le --channels=1 /tmp/test.wav`
- `pactl list short sources` 查看可用录音源

### 常驻 WebSocket Server 尝试（最终回退到 fork+exec）

#### 动机
sherpa-onnx-offline 每录音 fork+exec 一次，模型加载（尤其是 FireRed 1.2GB AED 模型）在每次调用中重新初始化 ONNX Runtime 计算图，耗时 ~3.2s。用 `sherpa-onnx-offline-websocket-server` 常驻进程可以省掉重复初始化。

#### 协议陷阱
WebSocket server 的 binary 消息格式是：
```
[int32_le sample_rate] [int32_le float_byte_count] [float_le PCM...]
```
**不是**原始 WAV 文件。发 WAV 会导致 sample_rate 被读成 `"RIFF"`（0x46464952=1.18e9），duration 被算成 6.8e-5 秒，触发 `payload too large` 拒绝。解决方案：解析 WAV header，int16→float 转换后按协议打包。

#### 崩溃问题
server 进程在 fcitx5 内 `fork()` 出来后不稳定：
- 端口打开成功（TCP probe 通过）
- ~2s 后进程崩溃（ECONNRESET）
- 降线程（30→4→1）、加 settle 等待、进程探活均无效
- 根因推测：fcitx5 多线程环境下 `fork()` 存在 mutex 继承、FD 泄漏等问题，子进程状态不确定

#### 结论
- **回退到 fork+exec 每录音**，简单可靠
- 后续考虑 systemd service 托管 server 进程（不在 fcitx5 进程内 fork）
- FireRed 常驻化预期可省 ~2s（计算图初始化），推理本身 ~1s
- zipformer（~100MB）模型小、doubao（云端）不受影响

### 64K 录音缓冲区（消除 drain）

#### 发现
- 原 4K 缓冲在 CX31993 burst 期需读 15 次，`stop()` 可能落在 burst 中间导致丢尾部数据
- 改成 **64K 缓冲区**（2s 音频量）后 `pa_simple_read` 阻塞读自然跨过硬件周期：fill 期阻塞 2s 等于一次完整 burst 投递
- `stop()` 置位后循环末次读一定在 fill 期阻塞过，数据完整
- n_reads 从 ~2000（4K）降到 ~1（64K），一次读就够了

#### 结论
显式 drain 逻辑（原有时间推算/计时检测方案）全部移除，大缓冲区 + 阻塞读本身既是同步也是收尾。

### Ctrl+CapsLock 模型切换

#### 设计
- **CapsLock 长按** → 语音录音识别（不变）
- **Ctrl+CapsLock** → 独立切换模式：方向键切换模型，不启动录音
- `switchActive_` 标志隔离两个模式，消除识别与切换的冲突

### Wayland 下 commitString 窗口绑定不可行

#### 发现
- `commitString()` 最终通过 `zwp_input_method_v1::commit_string` 发给合成器
- 合成器只路由到**当前键盘焦点 surface**，不认 fcitx5 内部的 IC UUID
- 即使 `findByUUID()` 返回正确 IC，commit 仍到焦点窗口
- 日志验证：`findByUUID` 返回 Firefox IC、`commitString` 调用成功，但文本到鼠标所在窗口

#### niri 窗口绑定方案
- `onActivate()` 时 `niri msg focused-window` 捕获窗口 ID
- 提交时 `niri msg action focus-window --id <captured>` → 等 150ms → `commitString` → `niri msg action focus-window --id <original>` 恢复焦点
- **代价**：提交瞬间焦点跳闪一次
- `capturedWinId_` 不随 `onDeactivate()` 清除（ASR 结果在 deactivate 后到）

### 豆包 API header 解析修正
- doubao_provider.cpp 中 `getHeader()` 匹配 `"\n" + name + ":"` 防止命中 `access-control-expose-headers` 值里的 `X-Api-Status-Code` 字串
- WAV cleanup 移到 detached 线程 lambda 中（RAII `Cleanup` struct），析构函数不删文件

### Python 包管理规则（全局）
写在 `~/.config/opencode/AGENTS.md`：
- 唯一允许用 `uv venv && uv pip install xxx`
- 禁止 pip install、pacman -S python-xxx、`uv pip install --system`

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
- **usec 是绝对时间（微秒），不是相对偏移**。需传入 `fcitx::now(CLOCK_MONOTONIC) + delay`
- `fcitx::now(clockid)` 获取当前时间：头文件 `<fcitx-utils/eventloopinterface.h>`
- 返回 `std::unique_ptr<EventSourceTime>`，reset 即取消
- callback 返回 false 表示一次性定时器

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
- **自动注册**: 各 provider .cpp 中用 static 初始化 lambda 调用 `registerFactory()`，编译链接即自动注册，无需手动在 adapter 中注册
- **静态库需要 `link_whole`**: 用 `link_with` 会导致静态初始化器被链接器丢弃，必须改 `link_whole` 才能保留全局 static 对象
- **运行时切换**: 激活后按 ←/h（上一）→/l（下一）切换 ASR 后端，自动保存为默认
- **配置系统**: 使用 `FCITX_CONFIGURATION` 宏 + `readAsIni/safeSaveAsIni` 持久化默认后端
  - 头文件: `<fcitx-config/configuration.h>`, `<fcitx-config/iniparser.h>`, `<fcitx-utils/i18n.h>`

### 常见 addon 参考
- clipboard: `fcitx5/src/modules/clipboard/` (Module 类型)
- quickphrase: `fcitx5/src/modules/quickphrase/` (PreInputMethod 阶段拦截)
- quwei: `fcitx5-quwei/` (简单 InputMethodEngine 入门示例)

### CapsLock 还原
- Wayland V1 下 fcitx5 无法拦截 CapsLock (compositor 先处理)
- **uinput**: 内核级虚键注入, 兼容所有 compositor。关键: 设备必须**常驻**（创建一次、反复发键），不能每次创建销毁——compositor 需要时间初始化虚拟键盘才能正确处理 modifier 切换
- XTest 只能影响 XWayland，Wayland 原生应用无效
