// (a) LogLine 各支持类型编码-解码往返测试
// 覆盖：char/int32/uint32/int64/uint64/float/double、编译期字符串字面量、
//       char const*、std::string（含内嵌 '\0'）、std::string_view
//       （含不带 NUL 结尾的 buffer —— 旧实现会越界读 1 字节）。
#include "test_util.hpp"

#include <limits>
#include <sstream>
#include <string>
#include <string_view>

using namespace slog;
using namespace slog_test;

// 渲染 LogLine 的消息部分
static std::string message_of(LogLine &line)
{
    std::ostringstream os;
    line.stringify(os, false);
    return slog_test::message_of(os.str());
}

template <typename T> static std::string stream_str(T const &value)
{
    std::ostringstream os;
    os << value;
    return os.str();
}

static void check_round_trip(LogLine &line, std::string const &expected)
{
    std::string const actual = message_of(line);
    if (actual != expected)
    {
        ++failures();
        std::cerr << "round-trip mismatch: expected=[" << expected << "] actual=[" << actual << "]"
                  << std::endl;
    }
}

int main()
{
    // 数值类型
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << 'A';
        check_round_trip(line, stream_str('A'));
    }
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << int32_t(-123456);
        check_round_trip(line, stream_str(int32_t(-123456)));
    }
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << uint32_t(4000000000u);
        check_round_trip(line, stream_str(uint32_t(4000000000u)));
    }
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << std::numeric_limits<int64_t>::min();
        check_round_trip(line, stream_str(std::numeric_limits<int64_t>::min()));
    }
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << std::numeric_limits<uint64_t>::max();
        check_round_trip(line, stream_str(std::numeric_limits<uint64_t>::max()));
    }
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << 0.25f;
        check_round_trip(line, stream_str(0.25f));
    }
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << 2.718281828459045;
        check_round_trip(line, stream_str(2.718281828459045));
    }

    // 编译期字符串字面量与 char const*
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << "hello " << "world";
        check_round_trip(line, "hello world");
    }
    {
        char const *cstr = "c-string value";
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << cstr;
        check_round_trip(line, "c-string value");
    }

    // std::string
    {
        std::string str = "std::string value";
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << str;
        check_round_trip(line, "std::string value");
    }

    // 内嵌 '\0' 的 std::string：编码-解码必须完整往返
    {
        std::string embedded;
        embedded = "ab";
        embedded.push_back('\0');
        embedded += "cd";
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << embedded;
        check_round_trip(line, embedded);
    }

    // 不带 NUL 结尾的 string_view：不允许越界读，内容须完整保留
    {
        char raw[3] = {'x', 'y', 'z'}; // 故意不含 NUL 终止符
        std::string_view const sv(raw, 3);
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << sv;
        check_round_trip(line, "xyz");
    }

    // 非 NUL 结尾 buffer 中内嵌 '\0' 的 string_view
    {
        char raw[5] = {'a', '\0', 'b', 'c', 'd'};
        std::string_view const sv(raw, 5);
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << sv;
        check_round_trip(line, std::string(std::string_view(raw, 5)));
    }

    // string_view 前缀视图
    {
        char arr[] = "abcdef";
        std::string_view const sv(arr, 3);
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << sv;
        check_round_trip(line, "abc");
    }

    // 空字符串（编码为空、解码为空）
    {
        std::string const empty = "";
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << empty;
        check_round_trip(line, "");
    }

    // 多个值混合
    {
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        int const count = 42;
        double const pi = 3.14159265;
        std::string const name = "SLog";
        line << "count=" << count << " pi=" << pi << " name=" << name;
        check_round_trip(line, "count=42 pi=3.14159 name=SLog");
    }

    // 长消息：触发栈缓冲到堆缓冲的扩容
    {
        std::string const long_text(500, '#');
        LogLine line(LogLevel::INFO, "test.cpp", "test_func", 42);
        line << long_text;
        check_round_trip(line, long_text);
    }

    if (failures() == 0)
        std::cout << "test_logline PASSED" << std::endl;
    return failures() == 0 ? 0 : 1;
}
