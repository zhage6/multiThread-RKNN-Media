#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace timing {

using Clock = std::chrono::steady_clock;

inline long long UsSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - start).count();
}

inline long long UsBetween(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count();
}

inline void Log(const char* fmt, ...)
{
    static std::mutex log_mtx;
    static FILE* fp = std::fopen("stream_timing.log", "a");

    if (fp == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mtx);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();

    std::fprintf(fp, "t=%lld ", static_cast<long long>(now_ms));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(fp, fmt, args);
    va_end(args);

    std::fprintf(fp, "\n");
    std::fflush(fp);
}

} // namespace timing
