# 测试直接编译 slog.cpp，以访问内部测试钩子（自定义 OutputWriter /
# initialize_with_writer / shutdown_logger，见 slog.cpp 末尾），
# 不改动 slog.hpp 的公开 API。

set(SLOG_TESTS
    test_logline
    test_initialize
    test_level_filter
    test_concurrency
    test_file_output
)

foreach(test_name IN LISTS SLOG_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}
    )
    target_link_libraries(${test_name} PRIVATE Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endforeach()
