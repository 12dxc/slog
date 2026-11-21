// (c) 级别过滤正确性 + 被过滤日志不再读取时钟（惰性 LogStream）测试
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace slog;
using namespace slog_test;

int main()
{
    shutdown_logger();

    auto capture = std::make_unique<CaptureWriter>();
    CaptureWriter *cap = capture.get();
    initialize_with_writer(std::move(capture));

    // ─── 级别过滤正确性 ───
    set_log_level(LogLevel::WARN);
    log_debug() << "FILTERED-debug";
    log_info() << "FILTERED-info";
    log_warn() << "KEPT-warn";
    log_error() << "KEPT-err";
    log_fatal() << "KEPT-fatal";

    CHECK(wait_for_count(*cap, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(cap->count() == 3); // 被过滤的消息不得出现

    auto lines = cap->lines();
    auto contains = [&lines](char const *marker) {
        return std::any_of(lines.begin(), lines.end(), [marker](std::string const &l) {
            return l.find(marker) != std::string::npos;
        });
    };
    CHECK(contains("KEPT-warn"));
    CHECK(contains("KEPT-err"));
    CHECK(contains("KEPT-fatal"));
    CHECK(!contains("FILTERED-debug"));
    CHECK(!contains("FILTERED-info"));

    // ─── 被过滤路径不再读取时钟 ───
    // 被过滤的 log_* 返回惰性空流：不构造 LogLine、不调用 system_clock::now()。
    // 通过测量平均耗时验证：过滤路径应显著快于完整路径（后者包含取时钟、
    // 编码、入队）。
    set_log_level(LogLevel::FATAL);
    constexpr int FILTERED_MSGS = 1000000;
    for (int i = 0; i < 10000; ++i)
        log_info() << i; // 预热
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < FILTERED_MSGS; ++i)
    {
        log_info() << "x" << i;
        do_not_optimize(i);
    }
    auto t1 = std::chrono::steady_clock::now();
    auto const filtered_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / FILTERED_MSGS;

    set_log_level(LogLevel::DEBUG);
    constexpr int ENABLED_MSGS = 50000;
    for (int i = 0; i < 1000; ++i)
        log_info() << i; // 预热
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < ENABLED_MSGS; ++i)
        log_info() << "x" << i;
    auto t3 = std::chrono::steady_clock::now();
    auto const enabled_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count() / ENABLED_MSGS;

    std::cout << "filtered path: " << filtered_ns << " ns/msg, enabled path: " << enabled_ns
              << " ns/msg" << std::endl;
    // 过滤路径至少快一倍（实测过滤路径约为个位数 ns，完整路径 ≥ 数十 ns）
    CHECK(filtered_ns * 2 < enabled_ns);

    shutdown_logger();

    if (failures() == 0)
        std::cout << "test_level_filter PASSED" << std::endl;
    return failures() == 0 ? 0 : 1;
}
