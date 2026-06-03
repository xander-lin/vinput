# Vinput 开发发现记录

## 2026-06-03

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

### PipeWire 录音效率问题 (核心)

- 使用 `pa_simple_read(4096)` 录音, wall=6.06s 仅录到 audio=4.10s (效率 67%)
- 逐次耗时: min=0ms, max=2005ms — PipeWire 每 ~2s 挂起一次 USB 源设备
- 挂起一次需要 2s 唤醒, 中间 reads 以 0ms "爆发"吐出缓冲数据
- 录音时长恰好卡在两个挂起周期间时, 尾巴丢失 (请求 1s 仅得 0.13s)
- **根因**: PipeWire `node/suspend-node.lua` 默认 5s idle 后挂起, USB 设备 (CX31993) 每次 wake-up 需 ~2s
- **解决**: 常驻录音流 — 构造时打开一条 `pa_simple` stream, 后台线程持续 reading
  - 录音活跃时收集; 不活跃时丢弃
  - 流只创建一次, 消除每轮 open 的 2s 惩罚 + 让 PipeWire 不挂起源

#### 录音时长不足 - 深入分析
- `pa_simple_new` 连接耗时仅 10ms, 不是瓶颈
- 首次 `pa_simple_read` 要等 PipeWire 建图 (48kHz→16kHz 重采样 + S24LE→S16LE)
- PipeWire 的 burst 模式: 每 ~2s 出一批 2s 数据, 批次间挂起
- `session.suspend-timeout-seconds` 默认 5s, 但 USB 设备底层另有 2s 的 GPIO 唤醒开销

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
