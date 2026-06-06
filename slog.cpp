/*
 * SLog - 实现
 * 现代 C++20 超低延迟日志库
 *
 * 许可证: MIT
 */

#include "slog.hpp"
#include <cstring>
#include <chrono>
#include <ctime>
#include <thread>
#include <tuple>
#include <atomic>
#include <queue>
#include <fstream>
#include <iostream>

#include <algorithm>

namespace
{
    // ─── 时间戳 ───────────────────────────────────────────────────────

    // 返回自 epoch 以来的秒数（使用 system_clock 获取真实日历时间）
    uint64_t timestamp_now()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // 格式化时间戳为 [YYYY-MM-DD HH:MM:SS]
    void format_timestamp(std::ostream &os, uint64_t timestamp)
    {
        std::time_t time_t = static_cast<std::time_t>(timestamp);
        auto tm = std::localtime(&time_t);
        char buffer[32];
        strftime(buffer, 32, "%Y-%m-%d %T", tm);
        os << '[' << buffer << ']';
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
        case LogLevel::ERROR:
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
        case LogLevel::ERROR:
            return "\033[31m";   // 红色
        case LogLevel::FATAL:
            return "\033[1;31m"; // 粗体红色
        }
        return "\033[0m";
    }

    // ANSI 重置码
    static constexpr const char *ANSI_RESET = "\033[0m";

    // ─── LogLine 编码 ─────────────────────────────────────────────────

    template <typename Arg>
    void LogLine::encode(Arg arg)
    {
        *reinterpret_cast<Arg *>(buffer()) = arg;
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
        encode<uint64_t>(timestamp_now());
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

    void LogLine::encode_c_string(char const *arg, size_t length)
    {
        if (length == 0)
            return;

        resize_buffer_if_needed(1 + length + 1);
        char *b = buffer();
        auto type_id = TupleIndex<char *, SupportedTypes>::value;
        *reinterpret_cast<uint8_t *>(b++) = static_cast<uint8_t>(type_id);
        memcpy(b, arg, length + 1);
        m_bytes_used += 1 + length + 1;
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

        // 依次读取头部字段
        uint64_t timestamp = *reinterpret_cast<uint64_t *>(b);
        b += sizeof(uint64_t);
        std::thread::id threadid = *reinterpret_cast<std::thread::id *>(b);
        b += sizeof(std::thread::id);
        string_literal_t file = *reinterpret_cast<string_literal_t *>(b);
        b += sizeof(string_literal_t);
        string_literal_t function = *reinterpret_cast<string_literal_t *>(b);
        b += sizeof(string_literal_t);
        uint32_t line = *reinterpret_cast<uint32_t *>(b);
        b += sizeof(uint32_t);
        LogLevel loglevel = *reinterpret_cast<LogLevel *>(b);
        b += sizeof(LogLevel);

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
        if (loglevel >= LogLevel::ERROR)
            os.flush();
    }

    // ─── 解码辅助函数 ─────────────────────────────────────────────────

    template <typename Arg>
    char *decode(std::ostream &os, char *b, Arg * /*dummy*/)
    {
        Arg arg = *reinterpret_cast<Arg *>(b);
        os << arg;
        return b + sizeof(Arg);
    }

    // 解码编译期字符串字面量
    template <>
    char *decode(std::ostream &os, char *b, LogLine::string_literal_t * /*dummy*/)
    {
        LogLine::string_literal_t s = *reinterpret_cast<LogLine::string_literal_t *>(b);
        os << s.m_s;
        return b + sizeof(LogLine::string_literal_t);
    }

    // 解码 C 风格字符串（逐字符输出直到 '\0'）
    template <>
    char *decode(std::ostream &os, char *b, char ** /*dummy*/)
    {
        while (*b != '\0')
        {
            os << *b;
            ++b;
        }
        return ++b;
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
            return m_write_state[size].fetch_add(1, std::memory_order_acquire) + 1 == size;
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
              m_flag(ATOMIC_FLAG_INIT), m_read_index(0)
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
            m_write_index.store(0, std::memory_order_relaxed);
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

    class ConsoleWriter : public OutputWriter
    {
    public:
        void write(LogLine &logline) override
        {
            logline.stringify(std::cout, true);
        }
    };

    // ─── 文件写入器：文件输出，自动滚动 ───────────────────────────────

    class FileWriter : public OutputWriter
    {
    public:
        FileWriter(std::string const &log_directory, std::string const &log_file_name,
                   uint32_t log_file_roll_size_mb)
            : m_log_file_roll_size_bytes(log_file_roll_size_mb * 1024 * 1024),
              m_name(log_directory + log_file_name)
        {
            roll_file();
        }

        void write(LogLine &logline) override
        {
            auto pos = m_os->tellp();
            logline.stringify(*m_os, false);
            m_bytes_written += m_os->tellp() - pos;
            if (m_bytes_written > m_log_file_roll_size_bytes)
            {
                roll_file();
            }
        }

    private:
        // 滚动到下一个日志文件
        void roll_file()
        {
            if (m_os)
            {
                m_os->flush();
                m_os->close();
            }

            m_bytes_written = 0;
            m_os.reset(new std::ofstream());
            std::string log_file_name = m_name;
            log_file_name.append(".");
            log_file_name.append(std::to_string(++m_file_number));
            log_file_name.append(".log");
            m_os->open(log_file_name, std::ofstream::out | std::ofstream::trunc);
        }

    private:
        uint32_t m_file_number = 0;
        std::streamoff m_bytes_written = 0;
        uint32_t const m_log_file_roll_size_bytes;
        std::string const m_name;
        std::unique_ptr<std::ofstream> m_os;
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
            m_state.store(State::SHUTDOWN);
            m_thread.join();
        }

        // 生产者接口：将日志行推入缓冲区
        void add(LogLine &&logline)
        {
            m_buffer.push(std::move(logline));
        }

        // 消费者线程：从缓冲区取出日志并写入输出
        void pop()
        {
            // 等待构造函数完成
            while (m_state.load(std::memory_order_acquire) == State::INIT)
                std::this_thread::sleep_for(std::chrono::microseconds(50));

            LogLine logline(LogLevel::INFO, nullptr, nullptr, 0);

            // 主循环：持续消费日志
            while (m_state.load() == State::READY)
            {
                if (m_buffer.try_pop(logline))
                    m_writer->write(logline);
                else
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }

            // 关闭前排空剩余日志
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
        QueueBuffer m_buffer;
        std::unique_ptr<OutputWriter> m_writer;
        std::thread m_thread;
    };

    // ═══════════════════════════════════════════════════════════════════
    //  全局状态
    // ═══════════════════════════════════════════════════════════════════

    std::unique_ptr<BackgroundLogger> g_logger;
    std::atomic<BackgroundLogger *> g_atomic_logger;
    std::atomic<unsigned int> g_loglevel = {0};

    // 将日志行推入后台缓冲区
    void push_logline(LogLine &&logline)
    {
        g_atomic_logger.load(std::memory_order_acquire)->add(std::move(logline));
    }

    // ─── 初始化（终端输出）────────────────────────────────────────────

    void initialize()
    {
        auto writer = std::make_unique<ConsoleWriter>();
        g_logger.reset(new BackgroundLogger(std::move(writer)));
        g_atomic_logger.store(g_logger.get(), std::memory_order_seq_cst);
    }

    // ─── 初始化（文件输出）────────────────────────────────────────────

    void initialize(std::string const &log_directory,
                    std::string const &log_file_name,
                    uint32_t log_file_roll_size_mb)
    {
        auto writer = std::make_unique<FileWriter>(log_directory, log_file_name, log_file_roll_size_mb);
        g_logger.reset(new BackgroundLogger(std::move(writer)));
        g_atomic_logger.store(g_logger.get(), std::memory_order_seq_cst);
    }

    // ─── 日志级别控制 ─────────────────────────────────────────────────

    void set_log_level(LogLevel level)
    {
        g_loglevel.store(static_cast<unsigned int>(level), std::memory_order_release);
    }

    bool is_logged(LogLevel level)
    {
        return static_cast<unsigned int>(level) >= g_loglevel.load(std::memory_order_relaxed);
    }

} // namespace slog
