// app/Log.h for the host tests: log lines to stdout, no crash handler.
#include "app/Log.h"

#include <cstdarg>
#include <cstdio>

namespace app
{
void logLine(const char* fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    std::printf("LOG ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}
void installCrashLog() {}
void nameThisThread(const char*) {}
} // namespace app
