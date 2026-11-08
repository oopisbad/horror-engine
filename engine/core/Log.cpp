#include "core/Log.hpp"

#include <cstdio>
#include <cstdlib>

#if !defined(NDEBUG) && defined(__linux__)
#include <execinfo.h>
#endif

namespace horror {

namespace {

const char *levelName(LogLevel lvl)
{
    switch (lvl) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    default:              return "?????";
    }
}

// Bare ANSI colors; harmless when the terminal ignores them.
const char *levelColor(LogLevel lvl)
{
    switch (lvl) {
    case LogLevel::Trace: return "\033[90m";
    case LogLevel::Debug: return "\033[36m";
    case LogLevel::Info:  return "\033[32m";
    case LogLevel::Warn:  return "\033[33m";
    case LogLevel::Error: return "\033[31m";
    case LogLevel::Fatal: return "\033[1;41m";
    default:              return "\033[0m";
    }
}

} // namespace

Logger &Logger::get()
{
    static Logger instance;
    return instance;
}

void logRaw(LogLevel lvl, const std::string_view &msg)
{
    // TODO(engine): route through pluggable sinks. Console for now.
    std::fprintf(stderr, "%s[%s] %.*s\033[0m\n", levelColor(lvl),
        levelName(lvl), static_cast<int>(msg.size()), msg.data());
    if (lvl == LogLevel::Fatal)
        std::fflush(stderr);
}

namespace detail {

void assertFail(const char *file, int line, const char *expr)
{
    std::string_view path(file);
    size_t slash = path.find_last_of('/');
    if (slash != std::string_view::npos)
        path = path.substr(slash + 1);

    std::fprintf(stderr, "\033[1;41mASSERT\033[0m %.*s:%d  `%s`\n",
        static_cast<int>(path.size()), path.data(), line, expr);
    std::fflush(stderr);

#if !defined(NDEBUG) && defined(__linux__)
    void *frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
#endif
    std::abort();
}

} // namespace detail
} // namespace horror
