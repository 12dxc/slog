# 基准测试：直接编译 slog.cpp 以使用内部测试钩子（shutdown_logger 控制排空时机）
add_executable(slog_bench bench.cpp)
target_include_directories(slog_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(slog_bench PRIVATE Threads::Threads)
