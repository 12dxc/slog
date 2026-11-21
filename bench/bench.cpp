// SLog 基准测试：测量生产者侧每条日志的平均开销（ns/msg，std::chrono 统计）
//
// 场景:
//   1. filtered   —— 被级别过滤的路径（惰性空流，不读取时钟、不编码、不入队）
//   2. single     —— 单线程 + 文件输出（含取时钟、编码、无锁队列入队）
//   3. multi 8T   —— 8 线程并发 + 文件输出（含无锁队列竞争）
//
// 说明: 输出走 FileWriter（后台消费者线程写文件），测量的是生产者侧开销；
//       队列缓冲区块链按需分配，短时突发不会使生产者自旋等待。
//       直接编译 slog.cpp 以便测试钩子 shutdown_logger() 控制排空时机。
#include "slog.cpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace slog;

namespace
{
    // 阻止编译器把测量循环优化掉
    template <typename T> inline void do_not_optimize(T const &value)
    {
        __asm__ __volatile__("" : : "r,m"(value) : "memory");
    }

    int64_t ns_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    }
} // namespace

int main()
{
    fs::path const dir = fs::temp_directory_path() / "slog_bench";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::cout << "SLog benchmark (producer-side average cost)" << std::endl;

    // ─── 1. 被级别过滤的路径 ──────────────────────────────────────────
    set_log_level(LogLevel::FATAL);
    constexpr int FILTERED_MSGS = 1000000;
    for (int i = 0; i < 100000; ++i)
        log_info() << i; // 预热
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < FILTERED_MSGS; ++i)
    {
        log_info() << "filtered " << i;
        do_not_optimize(i);
    }
    auto t1 = std::chrono::steady_clock::now();
    auto const filtered_ns = ns_between(t0, t1) / FILTERED_MSGS;

    // ─── 2. 单线程 + 文件输出 ─────────────────────────────────────────
    shutdown_logger();
    initialize(dir.string() + "/", "bench", 4096);
    set_log_level(LogLevel::DEBUG);
    constexpr int SINGLE_MSGS = 200000;
    for (int i = 0; i < 10000; ++i)
        log_info() << i; // 预热（未计时）
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < SINGLE_MSGS; ++i)
        log_info() << "bench message seq=" << i;
    auto t3 = std::chrono::steady_clock::now();
    auto const single_ns = ns_between(t2, t3) / SINGLE_MSGS;

    // ─── 3. 8 线程并发 + 文件输出 ─────────────────────────────────────
    constexpr int NUM_THREADS = 8;
    constexpr int MSGS_PER_THREAD = 100000;
    constexpr int MULTI_TOTAL = NUM_THREADS * MSGS_PER_THREAD;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&go]() {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < MSGS_PER_THREAD; ++i)
                log_info() << "bench thread message seq=" << i;
        });
    }
    auto t4 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    auto t5 = std::chrono::steady_clock::now();
    auto const multi_ns = ns_between(t4, t5) / MULTI_TOTAL;

    // 排空队列、关闭文件（未计时），然后清理临时文件
    shutdown_logger();
    fs::remove_all(dir);

    std::cout << "  filtered (level filter active): " << filtered_ns
              << " ns/msg   [" << FILTERED_MSGS << " msgs]" << std::endl;
    std::cout << "  single-thread (file output):    " << single_ns
              << " ns/msg   [" << SINGLE_MSGS << " msgs]" << std::endl;
    std::cout << "  8 threads (file output, agg):   " << multi_ns
              << " ns/msg   [" << MULTI_TOTAL << " msgs total]" << std::endl;

    return 0;
}
