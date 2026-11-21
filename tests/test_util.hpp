// 测试公共工具：直接编译 slog.cpp 以使用内部测试钩子，
// 提供 CaptureWriter（可捕获输出的自定义 writer）与断言/等待辅助。
#pragma once

#include "slog.cpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace slog_test
{
    inline int &failures()
    {
        static int count = 0;
        return count;
    }

#define CHECK(cond)                                                                                  \
    do                                                                                               \
    {                                                                                                \
        if (!(cond))                                                                                 \
        {                                                                                            \
            ++slog_test::failures();                                                                 \
            std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond          \
                      << std::endl;                                                                  \
        }                                                                                            \
    } while (0)

    // 阻止编译器把测量循环优化掉
    template <typename T> inline void do_not_optimize(T const &value)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        // MSVC 不支持 GCC 风格内联汇编：用 volatile 间接读取阻止消除
        void const *volatile sink = &value;
        static_cast<void>(sink);
#else
        __asm__ __volatile__("" : : "r,m"(value) : "memory");
#endif
    }

    // 可捕获输出的自定义 writer（write 仅由后台消费者线程调用）
    class CaptureWriter : public slog::OutputWriter
    {
    public:
        void write(slog::LogLine &logline) override
        {
            std::ostringstream os;
            logline.stringify(os, false);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lines.push_back(os.str());
        }

        size_t count()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_lines.size();
        }

        std::vector<std::string> lines()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_lines;
        }

    private:
        std::mutex m_mutex;
        std::vector<std::string> m_lines;
    };

    // 等待捕获的行数达到期望值（超时返回 false）
    inline bool wait_for_count(CaptureWriter &writer, size_t expected,
                               std::chrono::milliseconds timeout = std::chrono::seconds(30))
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (writer.count() < expected)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // 提取一行日志的消息部分（最后一个 "] " 之后的内容，去掉行尾换行）
    inline std::string message_of(std::string const &line)
    {
        auto const pos = line.rfind("] ");
        if (pos == std::string::npos)
            return line;
        std::string msg = line.substr(pos + 2);
        if (!msg.empty() && msg.back() == '\n')
            msg.pop_back();
        return msg;
    }
} // namespace slog_test
