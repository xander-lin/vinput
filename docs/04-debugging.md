# Fcitx5 调试指南

## 打印日志

使用 `FCITX_INFO()` 宏输出到日志：

```cpp
#include <fcitx-utils/log.h>

FCITX_INFO() << "key:" << keyEvent.key() << " isRelease:" << keyEvent.isRelease();
FCITX_DEBUG() << "state:" << someState;
FCITX_WARN() << "warning message";
FCITX_ERROR() << "error message";
```

该宏基于 `std::ostream`，支持所有标准 C++ 容器和 Fcitx 自定义类型（如 `RawConfig`、`Color`、`Key` 等）。

## GDB 调试

### 方法 1: 禁用输入法的终端

```bash
# 在不需要输入法的终端中启动
XMODIFIERS=@im=none xterm
# 在该 xterm 中启动 fcitx5 调试
gdb --args fcitx5 -r
```

### 方法 2: 禁用特定 frontend

```bash
# 禁用 dbus frontend 避免与 Gtk/Qt 冲突
gdb --args fcitx5 --disable=dbusfrontend -r
```

此时仍可用 xterm (XIM) 测试。

### 方法 3: 嵌套 X Server (推荐)

使用 Xephyr 避免焦点切换时状态重置：

```bash
Xephyr :1 &
DISPLAY=:1 openbox &
DISPLAY=:1 xterm &
DISPLAY=:1 gdb --args fcitx5 --disable=dbusfrontend -r
```

## 重启 fcitx5

```bash
fcitx5 -rd   # restart and debug
fcitx5 -r    # restart without debug output
```

## 测试框架

Fcitx 内建测试框架，可模拟键事件。在代码中包含：

```cpp
#include <fcitx-utils/testing.h>
```

参考: https://github.com/fcitx/fcitx5/blob/master/test/testquickphrase.cpp

使用 `setupTestingEnvironment` 设置测试环境：

```cpp
fcitx::setupTestingEnvironment(
    FCITX5_BINARY_DIR,
    {"testfrontend"},
    {"testim"},
    {FCITX5_SOURCE_DIR "/test"}
);
```

通过 EventDispatcher 在 fcitx 初始化后执行测试代码。

## 限定加载范围

在测试中可只加载特定 addon 以缩小调试范围：

```bash
fcitx5 --enable=myaddon,testfrontend
```

## 常见调试命令

```bash
# 查看日志
journalctl -u fcitx5 --since "5 min ago"

# 或查看标准输出
fcitx5 -r 2>&1 | tee /tmp/fcitx5.log
```
