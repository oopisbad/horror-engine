#pragma once

#include <atomic>
#include <format>
#include <string_view>

// Type-safe logging via std::format ("{}" placeholders), one global sink.
// Swappable later behind a sink interface (in-editor console, file sinks).
namespace horror {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal, Count };

// Free sink; declared before Logger so templates can call it unqualified.
void logRaw(LogLevel lvl, const std::string_view &msg);

class Logger {
public:
    // Per-process singleton; not thread-safe by design (main-thread only engine).
    static Logger &get();

    template<LogLevel L, typename... Args>
    static void log(const char *file, int line, std::format_string<Args...> fmt, Args &&...args)
    {
        Logger &l = get();
        if (static_cast<int>(L) < static_cast<int>(l.minLevel))
            return;
        std::string msg = std::vformat(fmt.get(), std::make_format_args(args...));
        std::string_view sv(msg);
        logRaw(L, sv);
    }

    LogLevel minLevel = LogLevel::Debug;
};

} // namespace horror

#define HOR_LOG_Q(L, ...)     ::horror::Logger::log<L>(__FILE__, __LINE__, __VA_ARGS__)
#define HOR_LOG_TRACE(...)   HOR_LOG_Q(::horror::LogLevel::Trace, __VA_ARGS__)
#define HOR_LOG_DEBUG(...)   HOR_LOG_Q(::horror::LogLevel::Debug,  __VA_ARGS__)
#define HOR_LOG_INFO(...)    HOR_LOG_Q(::horror::LogLevel::Info,  __VA_ARGS__)
#define HOR_LOG_WARN(...)    HOR_LOG_Q(::horror::LogLevel::Warn,  __VA_ARGS__)
#define HOR_LOG_ERROR(...)   HOR_LOG_Q(::horror::LogLevel::Error, __VA_ARGS__)
#define HOR_LOG_FATAL(...)   HOR_LOG_Q(::horror::LogLevel::Fatal, __VA_ARGS__)

// Logs once per call site (use for "this happens every frame" problems).
#define HOR_LOG_WARN_ONCE(...)                                              \
    do {                                                                    \
        static std::atomic<bool> hor_warn_once_flag{false};                 \
        if (!hor_warn_once_flag.exchange(true))                             \
            HOR_LOG_WARN(__VA_ARGS__);                                      \
    } while (0)

namespace horror {
namespace detail {
[[noreturn]] void assertFail(const char *file, int line, const char *expr);
} // namespace detail
} // namespace horror

// HOR_ASSERT: active in debug builds only (hot paths, cheap sanity checks).
// HOR_CHECK:  active in all builds -- use for engine-integrity conditions.
#if defined(NDEBUG)
#define HOR_ASSERT(expr) ((void)(expr))
#define HOR_CHECK(expr)  ((void)(expr))
#else
#define HOR_ASSERT(expr) ((expr) ? (void)0 : ::horror::detail::assertFail(__FILE__, __LINE__, #expr))
#define HOR_CHECK(expr)  ((expr) ? (void)0 : ::horror::detail::assertFail(__FILE__, __LINE__, #expr))
#endif
