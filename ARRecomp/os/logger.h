#pragma once

#include <fmt/core.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace logger
{
    enum class Level
    {
        Trace,
        Info,
        Warning,
        Error
    };

    inline Level g_minimumLevel = Level::Info;
    inline std::mutex g_mutex;

    template<typename... Args>
    void Print(Level level, const char* function, fmt::format_string<Args...> format, Args&&... args)
    {
        if (level < g_minimumLevel)
            return;

        static const char* prefixes[] = { "TRACE", "INFO ", "WARN ", "ERROR" };
        auto message = fmt::format(format, std::forward<Args>(args)...);

        std::lock_guard lock(g_mutex);
        fmt::print(stderr, "[{}] {}: {}\n", prefixes[int(level)], function, message);
        fflush(stderr);
    }
}

#define LOGF_TRACE(...) logger::Print(logger::Level::Trace, __func__, __VA_ARGS__)
#define LOGF_INFO(...) logger::Print(logger::Level::Info, __func__, __VA_ARGS__)
#define LOGF_WARNING(...) logger::Print(logger::Level::Warning, __func__, __VA_ARGS__)
#define LOGF_ERROR(...) logger::Print(logger::Level::Error, __func__, __VA_ARGS__)

// Logs the first call of an unimplemented function, so missing functionality is visible without flooding the log.
#define LOG_UNIMPLEMENTED() \
    do { \
        static std::atomic<bool> logged_; \
        if (!logged_.exchange(true)) \
            logger::Print(logger::Level::Warning, __func__, "unimplemented"); \
    } while (0)
