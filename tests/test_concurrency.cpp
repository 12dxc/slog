// (b) 多线程并发写压力测试：8 线程 × 10000 条，验证条数与内容完整性
#include "test_util.hpp"

#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace slog;
using namespace slog_test;

int main()
{
    shutdown_logger();

    constexpr int NUM_THREADS = 8;
    constexpr int MSGS_PER_THREAD = 10000;
    constexpr int TOTAL = NUM_THREADS * MSGS_PER_THREAD;

    auto capture = std::make_unique<CaptureWriter>();
    CaptureWriter *cap = capture.get();
    initialize_with_writer(std::move(capture));

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([t]() {
            for (int i = 0; i < MSGS_PER_THREAD; ++i)
                log_info() << "thread=" << t << " seq=" << i;
        });
    }
    for (auto &th : threads)
        th.join();

    // 等待消费者全部处理完毕
    CHECK(wait_for_count(*cap, TOTAL));
    CHECK(cap->count() == TOTAL); // 无丢失

    // 内容完整性：每个 (thread, seq) 恰好出现一次，无错乱
    std::unordered_set<std::string> expected;
    for (int t = 0; t < NUM_THREADS; ++t)
        for (int i = 0; i < MSGS_PER_THREAD; ++i)
            expected.insert("thread=" + std::to_string(t) + " seq=" + std::to_string(i));

    std::unordered_set<std::string> found;
    for (auto const &line : cap->lines())
        found.insert(message_of(line));

    CHECK(found == expected);

    shutdown_logger();

    if (failures() == 0)
        std::cout << "test_concurrency PASSED (" << TOTAL << " messages)" << std::endl;
    return failures() == 0 ? 0 : 1;
}
