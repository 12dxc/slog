#include "slog.hpp"
#include <cstdio>

using namespace std;

int main()
{
    SLOG(INFO) << "hello" << 123;
    SLOG(INFO) << "hello";

    getchar();
}