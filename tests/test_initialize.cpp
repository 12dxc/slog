// (d) initialize 幂等测试 + 未初始化/已关闭时空指针安全测试
#include "test_util.hpp"

#include <chrono>
#include <memory>
#include <thread>

using namespace slog;
using namespace slog_test;

int main()
{
    shutdown_logger(); // 确保从干净状态开始

    // 1) 首次初始化（自定义捕获 writer），日志正常写入
    auto capture1 = std::make_unique<CaptureWriter>();
    CaptureWriter *cap1 = capture1.get();
    initialize_with_writer(std::move(capture1));

    log_info() << "first-message";
    CHECK(wait_for_count(*cap1, 1));

    // 2) 重复 initialize() 必须被忽略：输出仍进入原有 capture writer。
    //    若未实现幂等（旧实现会 reset 掉旧 logger），后续日志会进入新的
    //    ConsoleWriter，capture 计数不再增长，本测试失败。
    initialize();
    log_info() << "second-message";
    CHECK(wait_for_count(*cap1, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(cap1->count() == 2);

    // 3) 显式关闭后，push_logline 不再解引用空指针（旧实现会段错误），
    //    日志被丢弃并输出 stderr 提示
    shutdown_logger();
    log_info() << "dropped-after-shutdown"; // 不应崩溃

    // 4) 显式 shutdown 之后允许重新初始化（测试钩子），日志恢复正常
    auto capture2 = std::make_unique<CaptureWriter>();
    CaptureWriter *cap2 = capture2.get();
    initialize_with_writer(std::move(capture2));
    log_info() << "after-reinit";
    CHECK(wait_for_count(*cap2, 1));
    CHECK(message_of(cap2->lines().at(0)) == "after-reinit");

    shutdown_logger();

    if (failures() == 0)
        std::cout << "test_initialize PASSED" << std::endl;
    return failures() == 0 ? 0 : 1;
}
