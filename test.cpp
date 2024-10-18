#include "slog.hpp"
#include <cstdio>

using namespace std;

int main()
{
    SLOG(INFO) << "hello";
    getchar();
}