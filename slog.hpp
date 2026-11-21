/*
 * SLog - 现代 C++20 超低延迟日志库
 *
 * 特性:
 *   - 流式打印:     log_info() << "value: " << x;
 *   - 纳秒精度时间戳
 *   - 终端彩色输出（ANSI 转义码，重定向时自动关闭）
 *   - 文件输出，自动滚动
 *   - 异步后台消费者线程
 *   - 日志永不丢失（保证模式）
 *   - 运行时日志级别控制
 *   - 无宏设计，使用 C++20 std::source_location
 *
 * 许可证: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <source_location>
#include <string>
#include <type_traits>

namespace slog
{

// 日志级别
// 注意：原枚举值 ERROR 与 <windows.h> 的 ERROR 宏冲突，已改名为 ERR
// （日志输出中的级别文本仍为 "ERROR"）。
enum class LogLevel : uint8_t
{
    DEBUG,
    INFO,
    WARN,
    ERR,
    FATAL
};

// ─── LogLine: 将日志数据编码到紧凑缓冲区 ───────────────────────────

class LogLine
{
  public:
    LogLine(LogLevel level, char const *file, char const *function, uint32_t line);
    ~LogLine();

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    // 将缓冲区数据序列化为文本输出（由消费者线程调用）
    void stringify(std::ostream &os, bool use_color);

    // 流式编码
    LogLine &operator<<(char arg);
    LogLine &operator<<(int32_t arg);
    LogLine &operator<<(uint32_t arg);
    LogLine &operator<<(int64_t arg);
    LogLine &operator<<(uint64_t arg);
    LogLine &operator<<(float arg);
    LogLine &operator<<(double arg);
    LogLine &operator<<(std::string const &arg);
    LogLine &operator<<(std::string_view arg);

    // 编译期字符串字面量（零拷贝）
    template <size_t N> LogLine &operator<<(const char (&arg)[N])
    {
        encode(string_literal_t(arg));
        return *this;
    }

    template <typename Arg>
        requires std::is_same_v<Arg, char const *>
    LogLine &operator<<(Arg const &arg)
    {
        encode(arg);
        return *this;
    }

    template <typename Arg>
        requires std::is_same_v<Arg, char *>
    LogLine &operator<<(Arg const &arg)
    {
        encode(arg);
        return *this;
    }

    // 编译期字符串字面量包装
    struct string_literal_t
    {
        explicit string_literal_t(char const *s) : m_s(s)
        {
        }
        char const *m_s;
    };

  private:
    char *buffer();

    template <typename Arg> void encode(Arg arg);

    template <typename Arg> void encode(Arg arg, uint8_t type_id);

    void encode(char *arg);
    void encode(char const *arg);
    void encode(string_literal_t arg);
    void encode_c_string(char const *arg, size_t length);
    void resize_buffer_if_needed(size_t additional_bytes);
    void stringify(std::ostream &os, char *start, char const *const end);

  private:
    size_t m_bytes_used;
    size_t m_buffer_size;
    std::unique_ptr<char[]> m_heap_buffer;
    // 256 字节栈缓冲区，短日志无需堆分配
    char m_stack_buffer[256 - 2 * sizeof(size_t) - sizeof(decltype(m_heap_buffer)) - 8];
};

// ─── 日志配置 ─────────────────────────────────────────────────────

void set_log_level(LogLevel level);
bool is_logged(LogLevel level);

// 终端颜色全局开关：true/false 强制开/关颜色，覆盖自动检测结果；
// 未调用时由 ConsoleWriter 自动检测（输出重定向时关闭颜色，Windows 启用 VT）
void set_console_colors_enabled(bool enabled);

// 初始化 - 终端输出
// 注意：logger 为进程级单例，只能成功初始化一次；
// 已初始化后再次调用会被忽略（不支持重初始化），应在任何 log_* 调用前完成。
void initialize();

// 初始化 - 文件输出
// log_directory:     日志目录，如 "./logs/"
// log_file_name:     文件名前缀，如 "myapp"
// log_file_roll_size_mb: 单个文件大小上限(MB)，超过后自动滚动
// 注意：logger 为进程级单例，只能成功初始化一次（重复调用被忽略）。
void initialize(std::string const &log_directory, std::string const &log_file_name, uint32_t log_file_roll_size_mb);

// ─── LogStream: RAII 流式日志包装器 ────────────────────────────────
// 活跃流：析构时自动将 LogLine 推入后台缓冲区
// 惰性空流：日志级别未达标时由 log_* 工厂返回 —— 不构造 LogLine、
//           不读取系统时钟，所有 operator<< 均为 no-op，析构不推送

void push_logline(LogLine &&logline);

class LogStream
{
  public:
    // 惰性空流：不持有 LogLine
    LogStream() noexcept = default;

    // 活跃流：接管一条已构造的 LogLine
    explicit LogStream(LogLine &&line) : m_line(std::move(line)), m_active(true)
    {
    }

    ~LogStream()
    {
        if (m_active)
            push_logline(std::move(*m_line));
    }

    // 移动构造：转移所有权，原对象不再推送
    LogStream(LogStream &&other) noexcept : m_line(std::move(other.m_line)), m_active(other.m_active)
    {
        other.m_active = false;
    }

    LogStream &operator=(LogStream &&) = delete;
    LogStream(LogStream const &) = delete;
    LogStream &operator=(LogStream const &) = delete;

    // 流式操作符：转发给内部 LogLine；惰性空流时为 no-op
    template <typename Arg> LogStream &operator<<(Arg &&arg)
    {
        if (m_line)
            *m_line << std::forward<Arg>(arg);
        return *this;
    }

  private:
    std::optional<LogLine> m_line;
    bool m_active = false;
};

// ─── 流式日志 API ─────────────────────────────────────────────────
// 用法: log_info() << "count=" << count;
// 级别未达标时返回惰性空流，开销接近零（不读取时钟、不编码）

[[nodiscard]] inline LogStream log_debug(std::source_location loc = std::source_location::current())
{
    if (!is_logged(LogLevel::DEBUG))
        return LogStream(); // 惰性空流
    return LogStream(LogLine(LogLevel::DEBUG, loc.file_name(), loc.function_name(), loc.line()));
}

[[nodiscard]] inline LogStream log_info(std::source_location loc = std::source_location::current())
{
    if (!is_logged(LogLevel::INFO))
        return LogStream(); // 惰性空流
    return LogStream(LogLine(LogLevel::INFO, loc.file_name(), loc.function_name(), loc.line()));
}

[[nodiscard]] inline LogStream log_warn(std::source_location loc = std::source_location::current())
{
    if (!is_logged(LogLevel::WARN))
        return LogStream(); // 惰性空流
    return LogStream(LogLine(LogLevel::WARN, loc.file_name(), loc.function_name(), loc.line()));
}

[[nodiscard]] inline LogStream log_error(std::source_location loc = std::source_location::current())
{
    if (!is_logged(LogLevel::ERR))
        return LogStream(); // 惰性空流
    return LogStream(LogLine(LogLevel::ERR, loc.file_name(), loc.function_name(), loc.line()));
}

[[nodiscard]] inline LogStream log_fatal(std::source_location loc = std::source_location::current())
{
    if (!is_logged(LogLevel::FATAL))
        return LogStream(); // 惰性空流
    return LogStream(LogLine(LogLevel::FATAL, loc.file_name(), loc.function_name(), loc.line()));
}

} // namespace slog
