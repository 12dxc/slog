# SLog

一个现代 C++20 超低延迟日志库。

**SLog** — 简洁、极速、实用。

## 特性

- **无宏设计**：使用 C++20 `std::source_location` 自动捕获调用位置
- **流式打印**：`log_info() << "value: " << x;`
- **终端彩色输出**：按日志级别显示不同颜色（ANSI 转义码）
- **文件输出**：自动滚动日志文件
- **异步架构**：后台消费者线程，生产者延迟极低
- **日志永不丢失**：队列缓冲区块链，保证模式
- **运行时日志级别控制**：DEBUG / INFO / WARN / ERROR / FATAL
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
    log_debug() << "这条不会被记录";
    log_warn()  << "这条会被记录";

    return 0;
}
```

## 构建

```bash
mkdir build && cd build
cmake ..
cmake --build .
./example
```

### Windows (MinGW)

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
./example.exe
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

### 初始化

```cpp
// 输出到终端
initialize();

// 输出到文件（自动滚动）
initialize("/var/log/", "myapp", 10);  // 目录, 文件名前缀, 滚动大小(MB)
```

### 运行时控制

```cpp
// 设置最低日志级别
set_log_level(LogLevel::WARN);

// 检查某级别是否启用
if (is_logged(LogLevel::DEBUG)) { ... }
```

## 日志输出格式

```
[2024-01-15 12:30:45][INFO][0x1a2b3c4d][main.cpp:main:15] Hello World
```

### 终端颜色

| 级别 | 颜色 |
|-------|-------|
| DEBUG | 灰色 |
| INFO | 绿色 |
| WARN | 黄色 |
| ERROR | 红色 |
| FATAL | 粗体红色 |

## 架构

```
生产者线程                  后台消费者线程
  log_info() << msg             │
       │                        │
       ▼                        │
  ┌──────────────┐              │
  │ QueueBuffer  │  ──pop──▶  OutputWriter
  │ (8MB 缓冲块)  │             ├─ ConsoleWriter（ANSI 颜色）
  │              │             └─ FileWriter（自动滚动）
  └──────────────┘
```

- **QueueBuffer**：8MB 缓冲区块链，日志永不丢失

## 设计亮点

- **无宏**：使用 `std::source_location` 自动捕获调用位置，无需 `#define`
- **RAII 流式包装**：`LogStream` 在析构时自动推送日志，保证异常安全
- **零拷贝字符串字面量**：编译期字符串直接存指针，不复制
- **栈优先缓冲区**：每条日志用 256 字节栈缓冲区，短日志无需堆分配
- **惰性转换**：数值以原始字节存储，消费者线程才转为字符串

## 许可证

MIT
