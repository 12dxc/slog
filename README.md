# SLog

一个现代 C++20 超低延迟日志库。

**SLog** — 简洁、极速、实用。

## 特性

- **无宏设计**：使用 C++20 `std::source_location` 自动捕获调用位置
- **流式打印**：`log_info() << "value: " << x;`
- **纳秒精度时间戳**：编码为 int64 纳秒，输出时格式化为 `[YYYY-MM-DD HH:MM:SS.nnnnnnnnn]`
- **终端彩色输出**：按日志级别显示不同颜色（ANSI 转义码；重定向时自动关闭）
- **文件输出**：自动滚动日志文件，文件名含启动时间戳
- **异步架构**：后台消费者线程，生产者延迟极低
- **日志永不丢失**：队列缓冲区块链，保证模式
- **运行时日志级别控制**：DEBUG / INFO / WARN / ERR / FATAL
- **零外部依赖**：仅使用 C++20 标准库

## 快速开始

```cpp
#include "slog.hpp"

using namespace slog;

int main()
{
    // 初始化终端输出
    initialize();

    // 流式打印
    log_info() << "Hello " << "World" << " count=" << 42;

    // 日志级别
    log_debug() << "调试信息";
    log_warn()  << "警告信息";
    log_error() << "错误信息";
    log_fatal() << "致命错误";

    // 运行时级别控制
    set_log_level(LogLevel::WARN);
    log_debug() << "这条不会被记录（惰性空流，不读取时钟）";
    log_warn()  << "这条会被记录";

    return 0;
}
```

> 注意：logger 是**进程级单例**，只能成功初始化一次；重复调用 `initialize()` 会被忽略，不支持重初始化。请在任何 `log_*` 调用之前完成初始化（未初始化时日志会被丢弃并输出 `std::cerr` 提示，不会崩溃）。

## 构建

```bash
mkdir build && cd build
cmake ..
cmake --build .
./example
```

### Windows (MinGW / clang64)

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
./example.exe
```

### 测试与基准

```bash
cmake .. && cmake --build .
ctest                      # 运行测试（tests/ 目录，5 个测试）
./bench/slog_bench.exe     # 运行基准测试
```

### 安装

```bash
cmake --install . --prefix /your/prefix
# 安装 lib/libslog.a 与 include/slog.hpp
```

## API 参考

### 流式日志

```cpp
log_debug() << "调试信息: " << value;
log_info()  << "信息: " << value;
log_warn()  << "警告: " << value;
log_error() << "错误: " << value;
log_fatal() << "致命错误: " << value;
```

- 全部标记为 `[[nodiscard]]`，返回的 `LogStream` 在语句结束时自动推送日志。
- 级别未达标时返回**惰性空流**：不构造 `LogLine`、不读取系统时钟、不编码、不入队，所有 `<<` 为 no-op，开销接近零。

### 初始化

```cpp
// 输出到终端（自动检测颜色支持）
initialize();

// 输出到文件（自动滚动）
initialize("/var/log/", "myapp", 10);  // 目录, 文件名前缀, 滚动大小(MB)
```

### 文件输出

文件名会附加**启动时间戳**，每次运行生成不同文件，不会覆盖上一次运行的滚动日志：

```
/var/log/myapp.20261121_223045.1.log   ← 本次运行的第 1 个滚动文件
/var/log/myapp.20261121_223045.2.log   ← 超过滚动大小后生成
```

超过单文件大小上限后自动滚动到 `.N.log`（N 递增）。若日志文件打开失败，会向 `stderr` 输出警告并停用文件输出（不会崩溃）。

### 运行时控制

```cpp
// 设置最低日志级别（枚举值：LogLevel::DEBUG/INFO/WARN/ERR/FATAL）
set_log_level(LogLevel::WARN);

// 检查某级别是否启用
if (is_logged(LogLevel::DEBUG)) { ... }
```

## 日志输出格式

```
[2024-01-15 12:30:45.123456789][INFO][0x1a2b3c4d][main.cpp:main:15] Hello World
```

### 终端颜色

| 级别 | 颜色 |
|-------|-------|
| DEBUG | 灰色 |
| INFO | 绿色 |
| WARN | 黄色 |
| ERROR | 红色 |
| FATAL | 粗体红色 |

## 线程安全承诺

- `log_debug/log_info/log_warn/log_error/log_fatal`、`set_log_level`、`is_logged` 可从任意数量的线程并发调用。
- 后台消费者线程独占输出 writer，多线程日志的行内内容不会交错。
- `initialize()` 幂等：首次调用生效，后续调用被忽略（进程级单例，不可重初始化）。
- **运行中**：队列缓冲区满时生产者**自旋等待**（背压模式），日志**不丢失**。
- **shutdown 之后**（进程退出析构）：不再接受新日志，新日志被丢弃并向 `stderr` 输出提示。
- 析构时先等待所有 in-flight 生产者完成入队，再排空队列，退出路径无日志丢失竞态。

## 空闲轮询的权衡

后台消费者线程在队列空闲时以 **50μs 轮询睡眠**代替条件变量 / atomic wait 唤醒：生产者路径零额外开销、实现简单，代价是低流量下每条日志最多引入约 50μs 的输出延迟与轻微的空闲 CPU 占用。改用唤醒机制可消除该开销，但会增加实现复杂度（超出当前范围）。

## 性能

生产者侧每条日志的平均开销（`std::chrono` 实测，见 `bench/bench.cpp`）：

| 场景 | 平均开销 |
|------|---------|
| 被级别过滤（惰性空流） | ~22 ns/msg |
| 单线程 + 文件输出 | ~141 ns/msg |
| 8 线程并发 + 文件输出（聚合） | ~130–205 ns/msg |

测量环境：

- OS：Windows 10 x64（10.0.19045）
- CPU：Intel Core i7-14650HX（16 核 24 线程）
- 编译器：clang 23.1.4（MSYS2 clang64，`x86_64-w64-windows-gnu`），Release `-O3 -std=c++20`
- 输出方式：`FileWriter` 写入本地 SSD 临时目录；测量的是**生产者侧**开销（含取时钟、编码、无锁队列入队），文件写入由后台消费者线程完成
- 线程数：多线程场景为 8 个生产者线程 × 100000 条；单线程 200000 条；过滤路径 1000000 条

> 数字为真实运行结果，随机器负载 / 频率波动，仅供量级参考。请以 `./bench/slog_bench.exe` 在您自己的环境中测得的数据为准。

## 平台支持

- **Windows**：日志级别枚举值使用 `LogLevel::ERR` 而非 `ERROR`，因为 `<windows.h>` 将 `ERROR` 定义为宏（这是破坏性 API 变更；日志输出中的级别文本仍为 `ERROR`）。`DEBUG` 与宏无冲突，保留。
- **Windows 控制台颜色**：ANSI 转义码需要 Windows 10+ 终端支持；SLog 会自动调用 `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` 启用 VT 处理。输出被重定向（非终端）时自动关闭颜色。也可用 `set_console_colors_enabled(true/false)` 强制开/关颜色。
- **时间转换**：使用线程安全的 `localtime_s`（Windows）/ `localtime_r`（POSIX），不使用非可重入的 `std::localtime`。
- **Linux / macOS**：完整支持。

## 架构

```
生产者线程                  后台消费者线程
  log_info() << msg             │
       │                        │
       ▼                        │
  ┌──────────────┐              │
  │ QueueBuffer  │  ──pop──▶  OutputWriter
  │ (8MB 缓冲块)  │             ├─ ConsoleWriter（ANSI 颜色，可开关）
  │              │             └─ FileWriter（自动滚动，含启动时间戳）
  └──────────────┘
```

- **QueueBuffer**：8MB 缓冲区块链，日志永不丢失
- **LogLine**：日志内容编码到紧凑字节缓冲区（头部 37 字节 + 类型标记的消息体），数值以原始字节存储，消费者线程才转为字符串（惰性格式化）

## 设计亮点

- **无宏**：使用 `std::source_location` 自动捕获调用位置，无需 `#define`
- **RAII 流式包装**：`LogStream` 在析构时自动推送日志，保证异常安全
- **惰性空流**：级别未达标时不构造 LogLine、不读取时钟，过滤路径接近零开销
- **零拷贝字符串字面量**：编译期字符串直接存指针，不复制
- **栈优先缓冲区**：每条日志用 256 字节栈缓冲区，短日志无需堆分配
- **惰性转换**：数值以原始字节存储，消费者线程才转为字符串
- **字节安全编解码**：全部通过 `std::memcpy` 写入/读出（严格别名与对齐安全）；字符串按长度前缀编码，支持不带 NUL 结尾的 `string_view` 与内嵌 `\0` 的 `std::string`

## 致谢（Acknowledgments）

本项目的无锁缓冲区架构设计参考自 [mvorbrodt/blog](https://github.com/mvorbrodt/blog)（MIT License），在此致谢。

## 许可证

MIT
