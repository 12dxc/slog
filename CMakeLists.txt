cmake_minimum_required(VERSION 3.16)
project(SLog VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 线程库（std::thread 后台消费者线程需要）
find_package(Threads REQUIRED)

# SLog library (static)
add_library(slog STATIC slog.cpp)
target_include_directories(slog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(slog PUBLIC Threads::Threads)

# Example executable
add_executable(example example.cpp)
target_link_libraries(example PRIVATE slog)

# Tests (CTest)
enable_testing()
add_subdirectory(tests)

# Benchmark
add_subdirectory(bench)

# Install rules (静态库 + 头文件)
include(GNUInstallDirs)
install(TARGETS slog ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES slog.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
