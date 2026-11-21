// (e) 文件输出端到端测试：FileWriter 写入临时目录后读回校验行数与内容
#include "test_util.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

using namespace slog;
using namespace slog_test;

int main()
{
    shutdown_logger();

    fs::path const dir = fs::temp_directory_path() / "slog_test_file_output";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string const dir_str = dir.string() + "/";

    constexpr int LINES = 200;
    initialize(dir_str, "app", 64); // 64MB 滚动上限，本测试不会触发滚动
    for (int i = 0; i < LINES; ++i)
        log_info() << "file-test seq=" << i;

    // shutdown_logger 排空队列并关闭文件（ofstream 析构刷新缓冲），
    // 保证随后读回的数据完整
    shutdown_logger();

    // 收集本次运行生成的日志文件
    std::vector<fs::path> log_files;
    for (auto const &entry : fs::directory_iterator(dir))
        if (entry.is_regular_file() && entry.path().extension() == ".log")
            log_files.push_back(entry.path());

    CHECK(log_files.size() == 1);
    if (log_files.size() == 1)
    {
        std::string const name = log_files[0].filename().string();
        // 文件名应形如 app.YYYYMMDD_HHMMSS.1.log（含启动时间戳，不覆盖上次运行）
        CHECK(name.rfind("app.", 0) == 0);
        CHECK(name.find(".1.log") != std::string::npos);

        std::ifstream input(log_files[0]);
        CHECK(input.is_open());
        std::unordered_set<int> seqs;
        int non_empty_lines = 0;
        std::string line;
        while (std::getline(input, line))
        {
            if (line.empty())
                continue;
            ++non_empty_lines;
            std::string const msg = message_of(line);
            if (msg.rfind("file-test seq=", 0) == 0)
                seqs.insert(std::atoi(msg.c_str() + std::strlen("file-test seq=")));
        }
        CHECK(non_empty_lines == LINES); // 行数校验
        CHECK(seqs.size() == static_cast<size_t>(LINES)); // 内容完整性校验
    }

    fs::remove_all(dir);

    if (failures() == 0)
        std::cout << "test_file_output PASSED (" << LINES << " lines)" << std::endl;
    return failures() == 0 ? 0 : 1;
}
