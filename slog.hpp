#pragma once

#include "ConcurrentQueue.h"
#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <print>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

// 日志等级
enum LogLevel
{
    DEBUG,
    INFO,
    ERROR,
    FATAL,
};

// 日志等级打印字符串
const std::array<std::string_view, sizeof(LogLevel)> level_strings = {
    "DEBUG",
    "INFO",
    "ERROR",
    "FATAL",
};

// 日志等级颜色
const std::array<std::string_view, sizeof(LogLevel)> level_colors = {
    "\033[32m", // 绿色
    "\033[36m", // 青色
    "\033[31m", // 红色
    "\033[41m", // 红色背景
};

inline ConcurrentQueue<std::string> g_log_que;

class Logger
{
  public:
    Logger(LogLevel level, std::source_location const &sl) noexcept
        : level_(level), sl_(sl), time_ss_()
    {
        const auto now = std::chrono::system_clock::now();
        const time_t t_c = std::chrono::system_clock::to_time_t(now);
        time_ss_ << std::put_time(std::localtime(&t_c), "%Y-%m-%d %H:%M:%S");
    }

    Logger &operator<<(auto msg)
    {
        log(msg);
        return *this;
    }

    void log(auto const &msg) const
    {
        g_log_que.push(std::format(
            "{} {}", formatLoglevel(), formatErrorMessage(msg)));
    }

  private:
    // 格式化日志等级
    std::string formatLoglevel() const
    {
        return std::format(
            "{}[{}]{}",
            level_colors.at(level_),
            level_strings.at(level_),
            "\033[0m");
    }

    // 格式化错误信息
    std::string formatErrorMessage(auto const &msg) const
    {
        return std::format(
            "{} {}:{}> {}",
            time_ss_.str(),
            sl_.file_name(),
            sl_.line(),
            msg);
    }

    LogLevel level_;
    std::source_location sl_;
    mutable std::ostringstream time_ss_;
};

/* 启动异步写线程 内部包含写逻辑 */
static void startLoggingThread()
{
    std::thread logging_thread(
        []()
        {
            while (true)
            {
                std::string str_log_mes;
                str_log_mes = g_log_que.waitPop();
                std::println("{}", str_log_mes);
            }
        });
    logging_thread.detach();
}

inline Logger SLOG(
    LogLevel level,
    std::source_location sl = std::source_location::current())
{
    static bool called = false;
    if (!called)
    {
        startLoggingThread();
        called = true;
    }
    Logger logger(level, sl);
    return logger;
}

#define SLOG_DEBUG SLOG(DEBUG)
#define SLOG_INFO SLOG(INFO)
#define SLOG_ERROR SLOG(ERROR)
#define SLOG_FATAL SLOG(FATAL)
