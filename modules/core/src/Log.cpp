// The log's stdio writer and the optional application sink.
#include "aver/core/Log.hpp"

#include <cstdio>
#include <mutex>

namespace aver {

namespace {
// The one mutex guarding both the stdio write and the sink pointer.
std::mutex& logMutex() { static std::mutex m; return m; }
LogSinkFn   g_sink    = nullptr;
void*       g_sinkCtx = nullptr;
} // namespace

// Installs or removes the application's log sink.
void setLogSink(LogSinkFn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(logMutex());
    g_sink    = fn;
    g_sinkCtx = ctx;
}

namespace detail {

// The five-character tag printed for a level.
static const char* levelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// Writes one formatted line to stdio and then to the sink, both under the log mutex.
void logWrite(LogLevel level, std::string_view message) {
    std::lock_guard<std::mutex> lock(logMutex());
    std::FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "[%s] %.*s\n", levelTag(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(out);
    if (g_sink) g_sink(g_sinkCtx, level, message);
}

} // namespace detail
} // namespace aver
