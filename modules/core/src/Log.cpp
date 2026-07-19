#include "aver/core/Log.hpp"

#include <cstdio>
#include <mutex>

namespace aver::detail {

static const char* levelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void logWrite(LogLevel level, std::string_view message) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    std::FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "[%s] %.*s\n", levelTag(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(out);
}

} // namespace aver::detail
