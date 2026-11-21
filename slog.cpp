/*
 * SLog - 实现
 * 现代 C++20 超低延迟日志库
 *
 * 许可证: MIT
 */

#include "slog.hpp"
#include <cstring>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <tuple>
#include <atomic>
#include <queue>
#include <fstream>
#include <iostream>

#include <algorithm>

#if defined(_WIN32)
// 注意：必须在 windows.h 之前定义 NOMINMAX，避免 min/max 宏污染 std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
    // ─── 时间戳 ───────────────────────────────────────────────────────

    // 返回自 epoch 以来的纳秒数（int64，使用 system_clock 获取真实日历时间）。
    // 以纳秒精度编码进日志行，输出时再拆分为秒 + 亚秒。
    int64_t timestamp_now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // 线程安全的时间转换（Windows: localtime_s；POSIX: localtime_r），失败返回 false
    bool thread_safe_localtime(std::time_t time, std::tm &out)
    {
#ifdef _WIN32
        return ::localtime_s(&out, &time) == 0;
#else
        return ::localtime_r(&time, &out) != nullptr;
#endif
    }

    // 格式化纳秒时间戳为 [YYYY-MM-DD HH:MM:SS.nnnnnnnnn]
    void format_timestamp(std::ostream &os, uint64_t timestamp_ns)
    {
        std::time_t const seconds = static_cast<std::time_t>(timestamp_ns / 1'000'000'000ULL);
        uint32_t const subsec = static_cast<uint32_t>(timestamp_ns % 1'000'000'000ULL);

        std::tm tm_buf = {};
        char time_buffer[32] = "";
        if (thread_safe_localtime(seconds, tm_buf) &&
            std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %T", &tm_buf) > 0)
        {
            char subsec_buffer[16];
            std::snprintf(subsec_buffer, sizeof(subsec_buffer), "%09u", subsec);
            os << '[' << time_buffer << '.' << subsec_buffer << ']';
        }
        else
        {
            os << "[time-error]";
        }
    }

    // 从完整路径中提取文件名
    char const *filename_only(char const *path)
    {
        if (!path) return "";
        char const *p = path;
        char const *last = path;
        while (*p) {
            if (*p == '/' || *p == '\\') last = p + 1;
            ++p;
        }
        return last;
    }

    // 生成 "YYYYMMDD_HHMMSS" 形式的启动时间戳，用于日志文件命名，
    // 避免每次运行覆盖上一次运行的滚动文件
    std::string startup_timestamp()
    {
        std::time_t const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_buf = {};
        char buffer[32] = "";
        if (thread_safe_localtime(now, tm_buf))
            std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm_buf);
        else
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(now));
        return buffer;
    }

    // 获取当前线程 ID（thread_local 缓存）
    std::thread::id this_thread_id()
    {
        static thread_local const std::thread::id id = std::this_thread::get_id();
        return id;
    }

    // ─── TupleIndex 辅助模板 ──────────────────────────────────────────
    // 在 tuple 中查找类型 T 的索引，用于 encode/decode 的 type_id

    template <typename T, typename Tuple>
    struct TupleIndex;

    template <typename T, typename... Types>
    struct TupleIndex<T, std::tuple<T, Types...>>
    {
        static constexpr const std::size_t value = 0;
    };

    template <typename T, typename U, typename... Types>
    struct TupleIndex<T, std::tuple<U, Types...>>
    {
        static constexpr const std::size_t value = 1 + TupleIndex<T, std::tuple<Types...>>::value;
    };

} // anonymous namespace

namespace slog
{
    // ─── 流式编码支持的类型列表 ───────────────────────────────────────

    typedef std::tuple<char, uint32_t, uint64_t, int32_t, int64_t, float, double,
                       LogLine::string_literal_t, char *>
        SupportedTypes;

    // ─── 日志级别转字符串 ─────────────────────────────────────────────

    char const *to_string(LogLevel loglevel)
    {
        switch (loglevel)
        {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERR:
            return "ERROR";
        case LogLevel::FATAL:
            return "FATAL";
        }
        return "?????";
    }

    // ─── ANSI 颜色码 ──────────────────────────────────────────────────

    char const *level_color(LogLevel loglevel)
    {
        switch (loglevel)
        {
        case LogLevel::DEBUG:
            return "\033[90m";   // 灰色
        case LogLevel::INFO:
            return "\033[32m";   // 绿色
        case LogLevel::WARN:
            return "\033[33m";   // 黄色
        case LogLevel::ERR:
            return "\033[31m";   // 红色
        case LogLevel::FATAL:
            return "\033[1;31m"; // 粗体红色
        }
        return "\033[0m";
    }

    // ANSI 重置码
    static constexpr const char *ANSI_RESET = "\033[0m";

    // ─── LogLine 编码 ─────────────────────────────────────────────────

    // 通过 std::memcpy 写入字节缓冲区：避免 reinterpret_cast 的严格别名违规
    // 与未对齐访问（头部 37 字节之后写入 int64/double 等无对齐保证）。
    template <typename Arg>
    void LogLine::encode(Arg arg)
    {
        static_assert(std::is_trivially_copyable_v<Arg>,
                      "LogLine::encode requires trivially copyable types");
        std::memcpy(buffer(), &arg, sizeof(Arg));
        m_bytes_used += sizeof(Arg);
    }

    template <typename Arg>
    void LogLine::encode(Arg arg, uint8_t type_id)
    {
        resize_buffer_if_needed(sizeof(Arg) + sizeof(uint8_t));
        encode<uint8_t>(type_id);
        encode<Arg>(arg);
    }

    // 构造函数：编码时间戳、线程ID、文件名、函数名、行号、日志级别
    LogLine::LogLine(LogLevel level, char const *file, char const *function, uint32_t line)
        : m_bytes_used(0), m_buffer_size(sizeof(m_stack_buffer))
    {
        encode<uint64_t>(static_cast<uint64_t>(timestamp_now()));
        encode<std::thread::id>(this_thread_id());
        encode<string_literal_t>(string_literal_t(file));
        encode<string_literal_t>(string_literal_t(function));
        encode<uint32_t>(line);
        encode<LogLevel>(level);
    }

    LogLine::~LogLine() = default;

    // ─── 流式 operator<< 重载 ─────────────────────────────────────────

    LogLine &LogLine::operator<<(char arg)
    {
        encode<char>(arg, TupleIndex<char, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(int32_t arg)
    {
        encode<int32_t>(arg, TupleIndex<int32_t, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(uint32_t arg)
    {
        encode<uint32_t>(arg, TupleIndex<uint32_t, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(int64_t arg)
    {
        encode<int64_t>(arg, TupleIndex<int64_t, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(uint64_t arg)
    {
        encode<uint64_t>(arg, TupleIndex<uint64_t, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(float arg)
    {
        encode<float>(arg, TupleIndex<float, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(double arg)
    {
        encode<double>(arg, TupleIndex<double, SupportedTypes>::value);
        return *this;
    }

    LogLine &LogLine::operator<<(std::string const &arg)
    {
        encode_c_string(arg.c_str(), arg.length());
        return *this;
    }

    LogLine &LogLine::operator<<(std::string_view arg)
    {
        encode_c_string(arg.data(), arg.length());
        return *this;
    }

    // ─── 内部缓冲区管理 ───────────────────────────────────────────────

    char *LogLine::buffer()
    {
        return !m_heap_buffer ? &m_stack_buffer[m_bytes_used] : &(m_heap_buffer.get())[m_bytes_used];
    }

    // 按需扩展缓冲区
    void LogLine::resize_buffer_if_needed(size_t additional_bytes)
    {
        size_t const required_size = m_bytes_used + additional_bytes;
        if (required_size <= m_buffer_size)
            return;

        if (!m_heap_buffer)
        {
            // 首次从栈切换到堆
            m_buffer_size = std::max(static_cast<size_t>(512), required_size);
            m_heap_buffer.reset(new char[m_buffer_size]);
            memcpy(m_heap_buffer.get(), m_stack_buffer, m_bytes_used);
        }
        else
        {
            // 堆缓冲区扩容（2倍增长）
            m_buffer_size = std::max(static_cast<size_t>(2 * m_buffer_size), required_size);
            std::unique_ptr<char[]> new_heap_buffer(new char[m_buffer_size]);
            memcpy(new_heap_buffer.get(), m_heap_buffer.get(), m_bytes_used);
            m_heap_buffer.swap(new_heap_buffer);
        }
    }

    void LogLine::encode(char const *arg)
    {
        if (arg != nullptr)
            encode_c_string(arg, strlen(arg));
    }

    void LogLine::encode(char *arg)
    {
        if (arg != nullptr)
            encode_c_string(arg, strlen(arg));
    }

    // 编码 C 风格字符串：type_id(1) + 长度(8) + 原始字节。
    // 只拷贝 length 字节 —— 不要求 arg 以 NUL 结尾（string_view 可能不带
    // 终止符，旧实现 memcpy(length+1) 会越界读 1 字节）。
    // 长度前缀同时保证含内嵌 '\0' 的 std::string 能完整往返。
    void LogLine::encode_c_string(char const *arg, size_t length)
    {
        if (length == 0)
            return;

        resize_buffer_if_needed(1 + sizeof(uint64_t) + length);
        char *b = buffer();
        uint8_t const type_id = static_cast<uint8_t>(TupleIndex<char *, SupportedTypes>::value);
        std::memcpy(b, &type_id, sizeof(type_id));
        b += sizeof(type_id);
        uint64_t const length64 = static_cast<uint64_t>(length);
        std::memcpy(b, &length64, sizeof(length64));
        b += sizeof(length64);
        std::memcpy(b, arg, length);
        m_bytes_used += 1 + sizeof(uint64_t) + length;
    }

    void LogLine::encode(string_literal_t arg)
    {
        encode<string_literal_t>(arg, TupleIndex<string_literal_t, SupportedTypes>::value);
    }

    // ─── Stringify: 将缓冲区解码输出到 ostream ───────────────────────

    void LogLine::stringify(std::ostream &os, bool use_color)
    {
        char *b = !m_heap_buffer ? m_stack_buffer : m_heap_buffer.get();
        char const *const end = b + m_bytes_used;

        // 依次读取头部字段（std::memcpy 读出，避免严格别名违规与未对齐访问）
        uint64_t timestamp = 0;
        std::memcpy(&timestamp, b, sizeof(timestamp));
        b += sizeof(timestamp);
        std::thread::id threadid;
        std::memcpy(&threadid, b, sizeof(threadid));
        b += sizeof(threadid);
        string_literal_t file = string_literal_t(nullptr);
        std::memcpy(&file, b, sizeof(file));
        b += sizeof(file);
        string_literal_t function = string_literal_t(nullptr);
        std::memcpy(&function, b, sizeof(function));
        b += sizeof(function);
        uint32_t line = 0;
        std::memcpy(&line, b, sizeof(line));
        b += sizeof(line);
        LogLevel loglevel = LogLevel::DEBUG;
        std::memcpy(&loglevel, b, sizeof(loglevel));
        b += sizeof(loglevel);

        // 输出格式: [时间戳][级别][线程ID][文件:函数:行号] 消息
        if (use_color)
            os << level_color(loglevel);

        format_timestamp(os, timestamp);

        os << '[' << to_string(loglevel) << ']'
           << '[' << threadid << ']'
           << '[' << filename_only(file.m_s) << ':' << function.m_s << ':' << line << "] ";

        // 解码消息体
        stringify(os, b, end);

        if (use_color)
            os << ANSI_RESET;

        os << '\n';

        // ERROR 及以上级别立即刷新
        if (loglevel >= LogLevel::ERR)
            os.flush();
    }

    // ─── 解码辅助函数 ─────────────────────────────────────────────────

    template <typename Arg>
    char *decode(std::ostream &os, char *b, Arg * /*dummy*/)
    {
        static_assert(std::is_trivially_copyable_v<Arg>,
                      "LogLine::decode requires trivially copyable types");
        Arg arg;
        std::memcpy(&arg, b, sizeof(Arg)); // 避免严格别名违规与未对齐访问
        os << arg;
        return b + sizeof(Arg);
    }

    // 解码编译期字符串字面量
    template <>
    char *decode(std::ostream &os, char *b, LogLine::string_literal_t * /*dummy*/)
    {
        LogLine::string_literal_t s = LogLine::string_literal_t(nullptr);
        std::memcpy(&s, b, sizeof(s));
        os << s.m_s;
        return b + sizeof(LogLine::string_literal_t);
    }

    // 解码 C 风格字符串：按编码时写入的长度前缀输出（支持内嵌 '\0'）。
    // 旧实现按 '\0' 逐字符扫描，遇到内嵌 '\0' 会截断并把剩余字节误当作
    // type_id 解析。
    template <>
    char *decode(std::ostream &os, char *b, char ** /*dummy*/)
    {
        uint64_t length = 0;
        std::memcpy(&length, b, sizeof(length));
        b += sizeof(length);
        os.write(b, static_cast<std::streamsize>(length));
        return b + length;
    }

    // 递归解码消息体中的每个参数
    void LogLine::stringify(std::ostream &os, char *start, char const *const end)
    {
        if (start == end)
            return;

        uint8_t type_id = static_cast<uint8_t>(*start);
        start++;

        // 根据 type_id 解码对应类型
        switch (type_id)
        {
        case 0:
            stringify(os, decode(os, start, static_cast<std::tuple_element<0, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 1:
            stringify(os, decode(os, start, static_cast<std::tuple_element<1, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 2:
            stringify(os, decode(os, start, static_cast<std::tuple_element<2, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 3:
            stringify(os, decode(os, start, static_cast<std::tuple_element<3, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 4:
            stringify(os, decode(os, start, static_cast<std::tuple_element<4, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 5:
            stringify(os, decode(os, start, static_cast<std::tuple_element<5, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 6:
            stringify(os, decode(os, start, static_cast<std::tuple_element<6, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 7:
            stringify(os, decode(os, start, static_cast<std::tuple_element<7, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 8:
            stringify(os, decode(os, start, static_cast<std::tuple_element<8, SupportedTypes>::type *>(nullptr)), end);
            return;
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    //  缓冲区基础设施
    // ═══════════════════════════════════════════════════════════════════

    // ─── 自旋锁 ───────────────────────────────────────────────────────

    struct SpinLock
    {
        SpinLock(std::atomic_flag &flag) : m_flag(flag)
        {
            while (m_flag.test_and_set(std::memory_order_acquire))
                ;
        }
        ~SpinLock()
        {
            m_flag.clear(std::memory_order_release);
        }

    private:
        std::atomic_flag &m_flag;
    };

    // ─── 缓冲区块 ─────────────────────────────────────────────────────

    class Buffer
    {
    public:
        struct Item
        {
            Item(LogLine &&logline) : logline(std::move(logline)) {}
            char padding[256 - sizeof(LogLine)];
            LogLine logline;
        };

        static constexpr const size_t size = 32768; // 每块 8MB (32768 * 256 字节)

        Buffer() : m_buffer(static_cast<Item *>(std::malloc(size * sizeof(Item))))
        {
            for (size_t i = 0; i <= size; ++i)
            {
                m_write_state[i].store(0, std::memory_order_relaxed);
            }
            static_assert(sizeof(Item) == 256, "Unexpected size != 256");
        }

        ~Buffer()
        {
            unsigned int write_count = m_write_state[size].load();
            for (size_t i = 0; i < write_count; ++i)
            {
                m_buffer[i].~Item();
            }
            std::free(m_buffer);
        }

        // 写入指定位置，返回 true 表示需要切换到下一块
        bool push(LogLine &&logline, unsigned int const write_index)
        {
            new (&m_buffer[write_index]) Item(std::move(logline));
            m_write_state[write_index].store(1, std::memory_order_release);
            // acq_rel：release 侧保证本生产者此前的写入（含上面的发布 store）
            // 先于计数递增对其他生产者可见；acquire 侧保证触发换块的最后一个
            // 生产者能看到此前所有生产者对本块的发布写入。
            return m_write_state[size].fetch_add(1, std::memory_order_acq_rel) + 1 == size;
        }

        // 从指定位置读取
        bool try_pop(LogLine &logline, unsigned int const read_index)
        {
            if (m_write_state[read_index].load(std::memory_order_acquire))
            {
                Item &item = m_buffer[read_index];
                logline = std::move(item.logline);
                return true;
            }
            return false;
        }

        Buffer(Buffer const &) = delete;
        Buffer &operator=(Buffer const &) = delete;

    private:
        Item *m_buffer;
        std::atomic<unsigned int> m_write_state[size + 1];
    };

    // ─── 队列缓冲区（缓冲区块链，日志永不丢失）──────────────────────

    class QueueBuffer
    {
    public:
        QueueBuffer(QueueBuffer const &) = delete;
        QueueBuffer &operator=(QueueBuffer const &) = delete;

        QueueBuffer()
            : m_current_read_buffer{nullptr}, m_write_index(0),
              m_flag{}, m_read_index(0) // C++20: atomic_flag 默认即清零态
        {
            setup_next_write_buffer();
        }

        void push(LogLine &&logline)
        {
            unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed);
            if (write_index < Buffer::size)
            {
                if (m_current_write_buffer.load(std::memory_order_acquire)->push(std::move(logline), write_index))
                {
                    setup_next_write_buffer();
                }
            }
            else
            {
                // 当前缓冲区已满，等待切换
                while (m_write_index.load(std::memory_order_acquire) >= Buffer::size)
                    ;
                push(std::move(logline));
            }
        }

        bool try_pop(LogLine &logline)
        {
            if (m_current_read_buffer == nullptr)
                m_current_read_buffer = get_next_read_buffer();

            Buffer *read_buffer = m_current_read_buffer;
            if (read_buffer == nullptr)
                return false;

            if (read_buffer->try_pop(logline, m_read_index))
            {
                m_read_index++;
                if (m_read_index == Buffer::size)
                {
                    // 当前块读完，切换到下一块
                    m_read_index = 0;
                    m_current_read_buffer = nullptr;
                    SpinLock spinlock(m_flag);
                    m_buffers.pop();
                }
                return true;
            }
            return false;
        }

    private:
        // 设置下一块写入缓冲区
        void setup_next_write_buffer()
        {
            std::unique_ptr<Buffer> next_write_buffer(new Buffer());
            m_current_write_buffer.store(next_write_buffer.get(), std::memory_order_release);
            SpinLock spinlock(m_flag);
            m_buffers.push(std::move(next_write_buffer));
            // release（而非 relaxed）：保证 m_current_write_buffer 的发布先于
            // m_write_index 归零可见 —— 在"队列满"自旋的生产者一旦观察到
            // index < size，必然同时观察到新的写入块，避免向已满的旧块写入。
            m_write_index.store(0, std::memory_order_release);
        }

        // 获取下一块读取缓冲区
        Buffer *get_next_read_buffer()
        {
            SpinLock spinlock(m_flag);
            return m_buffers.empty() ? nullptr : m_buffers.front().get();
        }

    private:
        std::queue<std::unique_ptr<Buffer>> m_buffers;
        std::atomic<Buffer *> m_current_write_buffer;
        Buffer *m_current_read_buffer;
        std::atomic<unsigned int> m_write_index;
        std::atomic_flag m_flag;
        unsigned int m_read_index;
    };

    // ═══════════════════════════════════════════════════════════════════
    //  输出写入器
    // ═══════════════════════════════════════════════════════════════════

    // 输出写入器基类
    class OutputWriter
    {
    public:
        virtual ~OutputWriter() = default;
        virtual void write(LogLine &logline) = 0;
    };

    // ─── 终端写入器：带 ANSI 颜色的终端输出 ───────────────────────────

    // 终端颜色全局开关（set_console_colors_enabled 修改）：
    // -1 自动（isatty/VT 检测），0 强制关，1 强制开
    std::atomic<int> g_console_color_mode = {-1};

    class ConsoleWriter : public OutputWriter
    {
    public:
        // 颜色开关：
        //   std::nullopt（默认）→ 自动检测：输出被重定向（!isatty）时关闭颜色；
        //                         Windows 10+ 终端启用 VT 处理以支持 ANSI 转义码
        //   true / false        → 强制开 / 关颜色
        explicit ConsoleWriter(std::optional<bool> color = std::nullopt)
            : m_auto_use_color(color.has_value() ? *color : detect_color_support())
        {
        }

        void write(LogLine &logline) override
        {
            // 全局 setter 优先；未设置时使用构造时的自动检测结果
            int const mode = g_console_color_mode.load(std::memory_order_relaxed);
            bool const use_color = mode == 0 ? false : (mode == 1 ? true : m_auto_use_color);
            logline.stringify(std::cout, use_color);
        }

    private:
        static bool detect_color_support()
        {
#ifdef _WIN32
            // 输出被重定向到文件/管道时关闭颜色
            if (::_isatty(::_fileno(stdout)) == 0)
                return false;
            // Windows 10+ 需启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING 才能识别 ANSI 转义码
            HANDLE const handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr && ::GetConsoleMode(handle, &mode))
            {
                mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                return ::SetConsoleMode(handle, mode) != 0;
            }
            return false;
#else
            return ::isatty(::fileno(stdout)) == 1;
#endif
        }

        bool const m_auto_use_color;
    };

    // ─── 文件写入器：文件输出，自动滚动 ───────────────────────────────

    // 包装目标 streambuf 并统计写入字节数（替代每条日志两次 tellp 的开销）
    class CountingStreamBuf : public std::streambuf
    {
    public:
        void set_target(std::streambuf *target) { m_target = target; }
        std::streamsize count() const { return m_count; }
        void reset_count() { m_count = 0; }

    protected:
        std::streamsize xsputn(char const *s, std::streamsize n) override
        {
            m_count += n;
            return m_target ? m_target->sputn(s, n) : n;
        }

        int_type overflow(int_type ch) override
        {
            if (ch != traits_type::eof() && m_target)
            {
                m_count += 1;
                m_target->sputc(traits_type::to_char_type(ch));
            }
            return ch;
        }

        int sync() override { return m_target ? m_target->pubsync() : 0; }

    private:
        std::streambuf *m_target = nullptr;
        std::streamsize m_count = 0;
    };

    class FileWriter : public OutputWriter
    {
    public:
        FileWriter(std::string const &log_directory, std::string const &log_file_name,
                   uint32_t log_file_roll_size_mb)
            : // 64 位运算：uint32_t 直接乘 1024*1024 会在 ≥4096MB 时溢出为 0，
              // 导致每条日志都触发滚动
              m_log_file_roll_size_bytes(static_cast<uint64_t>(log_file_roll_size_mb) * 1024 *
                                         1024),
              // 文件名附加启动时间戳：每次运行使用不同文件，避免覆盖上次运行的滚动文件
              m_name(log_directory + log_file_name + "." + startup_timestamp()),
              m_out(&m_counting_buf)
        {
            m_counting_buf.set_target(m_os.rdbuf());
            roll_file();
        }

        void write(LogLine &logline) override
        {
            if (!m_os.is_open())
                return; // 打开失败时丢弃（构造时已输出过警告）

            m_counting_buf.reset_count();
            logline.stringify(m_out, false);
            m_bytes_written += m_counting_buf.count();
            if (m_bytes_written > m_log_file_roll_size_bytes)
            {
                roll_file();
            }
        }

    private:
        // 滚动到下一个日志文件
        void roll_file()
        {
            if (m_os.is_open())
            {
                m_os.flush();
                m_os.close();
            }

            m_bytes_written = 0;
            std::string log_file_name = m_name;
            log_file_name.append(".");
            log_file_name.append(std::to_string(++m_file_number));
            log_file_name.append(".log");
            m_os.open(log_file_name, std::ofstream::out | std::ofstream::trunc);
            if (!m_os.is_open())
            {
                std::cerr << "slog: failed to open log file '" << log_file_name
                          << "'; file logging is disabled" << std::endl;
            }
        }

        uint32_t m_file_number = 0;
        uint64_t m_bytes_written = 0;
        uint64_t const m_log_file_roll_size_bytes;
        std::string const m_name;
        std::ofstream m_os;
        CountingStreamBuf m_counting_buf;
        std::ostream m_out;
    };

    // ═══════════════════════════════════════════════════════════════════
    //  后台日志器：异步消费者线程
    // ═══════════════════════════════════════════════════════════════════

    class BackgroundLogger
    {
    public:
        explicit BackgroundLogger(std::unique_ptr<OutputWriter> writer)
            : m_state(State::INIT),
              m_writer(std::move(writer)),
              m_thread(&BackgroundLogger::pop, this)
        {
            m_state.store(State::READY, std::memory_order_release);
        }

        ~BackgroundLogger()
        {
            // 1) 停止接受新日志：此后 add() 丢弃并输出 stderr 提示
            m_stopped.store(true, std::memory_order_release);
            // 2) 等待所有 in-flight 生产者完成入队。否则排空循环可能与生产者
            //    竞态：生产者刚执行完 placement-new、尚未发布写状态，排空循环
            //    提前退出，随后析构释放 Buffer，导致日志丢失甚至未定义行为。
            while (m_in_flight.load(std::memory_order_acquire) != 0)
                std::this_thread::yield();
            // 3) 通知消费者排空剩余日志后退出
            m_state.store(State::SHUTDOWN, std::memory_order_release);
            m_thread.join();
        }

        // 生产者接口：将日志行推入缓冲区。
        // 运行期间队列满时自旋等待（日志不丢失）；shutdown 之后不再接受新日志
        // （丢弃并输出 stderr 提示）。
        void add(LogLine &&logline)
        {
            if (m_stopped.load(std::memory_order_acquire))
            {
                std::cerr << "slog: logger has been shut down; dropping log line" << std::endl;
                return;
            }
            m_in_flight.fetch_add(1, std::memory_order_relaxed);
            // 双重检查：覆盖"析构函数已置 stopped 并正在等待 in-flight 归零"的窗口
            if (m_stopped.load(std::memory_order_acquire))
            {
                m_in_flight.fetch_sub(1, std::memory_order_release);
                std::cerr << "slog: logger has been shut down; dropping log line" << std::endl;
                return;
            }
            m_buffer.push(std::move(logline));
            // release：保证 push 内部的写状态发布先于 in-flight 归零被析构观察到
            m_in_flight.fetch_sub(1, std::memory_order_release);
        }

        // 消费者线程：从缓冲区取出日志并写入输出
        void pop()
        {
            // 等待构造函数完成
            while (m_state.load(std::memory_order_acquire) == State::INIT)
                std::this_thread::sleep_for(std::chrono::microseconds(50));

            LogLine logline(LogLevel::INFO, nullptr, nullptr, 0);

            // 主循环：持续消费日志。
            // 权衡说明：队列空闲时以 50μs 轮询睡眠代替条件变量/atomic wait 唤醒 ——
            // 生产者路径零额外开销、实现简单，代价是低流量下每条日志最多引入
            // ~50μs 的输出延迟与轻微的空闲 CPU 占用。改用唤醒机制可消除该开销，
            // 但超出本次修改范围。
            while (m_state.load() == State::READY)
            {
                if (m_buffer.try_pop(logline))
                    m_writer->write(logline);
                else
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }

            // 关闭前排空剩余日志（此时所有 in-flight 生产者已完成入队）
            while (m_buffer.try_pop(logline))
            {
                m_writer->write(logline);
            }
        }

    private:
        enum class State
        {
            INIT,     // 初始化中
            READY,    // 就绪
            SHUTDOWN  // 关闭中
        };

        std::atomic<State> m_state;
        std::atomic<bool> m_stopped{false};      // shutdown 后不再接受新日志
        std::atomic<unsigned int> m_in_flight{0}; // 正在执行 add() 的生产者计数
        QueueBuffer m_buffer;
        std::unique_ptr<OutputWriter> m_writer;
        std::thread m_thread;
    };

    // ═══════════════════════════════════════════════════════════════════
    //  全局状态
    // ═══════════════════════════════════════════════════════════════════

    std::unique_ptr<BackgroundLogger> g_logger;
    std::atomic<BackgroundLogger *> g_atomic_logger = {nullptr};
    std::atomic<unsigned int> g_loglevel = {0};
    // initialize 幂等标记：logger 是进程级单例，禁止二次初始化
    //（旧实现在重复初始化时 reset 掉旧 logger，存在 use-after-free 窗口）
    std::atomic<bool> g_initialized = {false};

    // 将日志行推入后台缓冲区
    void push_logline(LogLine &&logline)
    {
        if (auto *logger = g_atomic_logger.load(std::memory_order_acquire))
            logger->add(std::move(logline));
        else
            std::cerr << "slog: logger not initialized; log message dropped "
                         "(call slog::initialize() first)" << std::endl;
    }

    // ─── 初始化（幂等）────────────────────────────────────────────────

    void initialize_impl(std::unique_ptr<OutputWriter> writer)
    {
        bool expected = false;
        if (!g_initialized.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
        {
            std::cerr << "slog: initialize() called again; ignored. The logger is a "
                         "process-wide singleton and cannot be re-initialized." << std::endl;
            return;
        }
        g_logger = std::make_unique<BackgroundLogger>(std::move(writer));
        g_atomic_logger.store(g_logger.get(), std::memory_order_release);
    }

    // ─── 初始化（终端输出）────────────────────────────────────────────

    void initialize()
    {
        initialize_impl(std::make_unique<ConsoleWriter>());
    }

    // ─── 初始化（文件输出）────────────────────────────────────────────

    void initialize(std::string const &log_directory,
                    std::string const &log_file_name,
                    uint32_t log_file_roll_size_mb)
    {
        initialize_impl(std::make_unique<FileWriter>(log_directory, log_file_name, log_file_roll_size_mb));
    }

    // ─── 日志级别控制 ─────────────────────────────────────────────────

    void set_log_level(LogLevel level)
    {
        g_loglevel.store(static_cast<unsigned int>(level), std::memory_order_release);
    }

    void set_console_colors_enabled(bool enabled)
    {
        g_console_color_mode.store(enabled ? 1 : 0, std::memory_order_relaxed);
    }

    bool is_logged(LogLevel level)
    {
        return static_cast<unsigned int>(level) >= g_loglevel.load(std::memory_order_relaxed);
    }

    // ═══ 以下为仅供测试使用的内部钩子（不在 slog.hpp 中声明）══════════

    // 测试钩子：以自定义 OutputWriter 替换全局 logger。
    // 仅可在确保没有其他线程正在写日志的单线程测试环境中调用。
    void initialize_with_writer(std::unique_ptr<OutputWriter> writer)
    {
        g_logger = std::make_unique<BackgroundLogger>(std::move(writer));
        g_atomic_logger.store(g_logger.get(), std::memory_order_release);
        g_initialized.store(true, std::memory_order_release);
    }

    // 测试钩子：关闭并销毁全局 logger（排空队列、刷新输出），随后可重新初始化
    void shutdown_logger()
    {
        g_logger.reset();
        g_atomic_logger.store(nullptr, std::memory_order_release);
        g_initialized.store(false, std::memory_order_release);
    }

} // namespace slog
