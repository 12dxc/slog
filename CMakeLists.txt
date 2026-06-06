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

# SLog library (static)
add_library(slog STATIC slog.cpp)
target_include_directories(slog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Example executable
add_executable(example example.cpp)
target_link_libraries(example PRIVATE slog)

# On Linux/macOS, link pthread
if(NOT WIN32)
    target_link_libraries(slog PRIVATE pthread)
    target_link_libraries(example PRIVATE pthread)
endif()
