/*
 * SLog - 演示程序
 *
 * 演示内容:
 *   1. 流式打印 (log_info() << ...)
 *   2. 终端彩色输出
 *   3. 多日志级别
 *   4. 运行时日志级别切换
 *   5. 多线程日志
 */
#include "slog.hpp"
#include <string>
#include <thread>
#include <vector>

using namespace slog;

void demo_stream_logging()
{
    log_info() << "=== 流式打印演示 ===";

    int count = 42;
    double pi = 3.14159265;
    std::string name = "SLog";

    log_debug() << "调试信息: count=" << count;
    log_info() << "信息: pi=" << pi << ", name=" << name;
    log_warn() << "警告: 磁盘使用率 " << 87 << "%";
    log_error() << "错误: 连接失败, 重试=" << 3;
    log_fatal() << "致命错误: 内存不足!";
}

void demo_log_levels()
{
    log_info() << "=== 日志级别演示 ===";

    log_debug() << "这是 DEBUG";
    log_info() << "这是 INFO";
    log_warn() << "这是 WARN";
    log_error() << "这是 ERROR";
    log_fatal() << "这是 FATAL";

    log_info() << "--- 切换到 WARN 级别 ---";
    set_log_level(LogLevel::WARN);

    log_debug() << "这条 DEBUG 不会出现";
    log_info() << "这条 INFO 不会出现";
    log_warn() << "这条 WARN 会出现";
    log_error() << "这条 ERROR 会出现";

    set_log_level(LogLevel::DEBUG);
    log_info() << "--- 切回 DEBUG 级别 ---";
}

void demo_multi_thread()
{
    log_info() << "=== 多线程日志演示 ===";

    constexpr int NUM_THREADS = 4;
    constexpr int MSGS_PER_THREAD = 5;

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([t, MSGS_PER_THREAD]() {
            for (int i = 0; i < MSGS_PER_THREAD; ++i)
            {
                log_info() << "线程 " << t << " 消息 " << (i + 1) << "/" << MSGS_PER_THREAD;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    for (auto &th : threads)
        th.join();
}

void demo_mixed_output()
{
    log_info() << "=== 综合输出演示 ===";

    log_info() << "开始批量处理...";

    int batch_id = 1001;
    int total = 5000;
    int processed = 1675;

    log_info() << "批次 #" << batch_id << ": 已处理 " << processed << "/" << total;
    log_warn() << "批次 #" << batch_id << ": 12 条记录因校验错误被跳过";
    log_error() << "批次 #" << batch_id << ": 3 条记录失败, 正在重试...";

    log_info() << "批量处理完成。";
}

int main()
{
    initialize();

    log_info() << "SLog 演示开始";
    log_info() << "";

    demo_stream_logging();
    log_info() << "";
    demo_log_levels();
    log_info() << "";
    demo_mixed_output();
    log_info() << "";
    demo_multi_thread();

    log_info() << "";
    log_info() << "SLog 演示结束";

    return 0;
}
