#include "aver/core/Log.hpp"

#include <cstdio>
#include <mutex>

namespace aver {

namespace {
// One mutex guards both the stdio write and the sink pointer, so a sink can be installed or removed
// without racing a concurrent log from another thread.
std::mutex& logMutex() { static std::mutex m; return m; }
LogSinkFn   g_sink    = nullptr;
void*       g_sinkCtx = nullptr;
} // namespace

void setLogSink(LogSinkFn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(logMutex());
    g_sink    = fn;
    g_sinkCtx = ctx;
}

namespace detail {

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
    std::lock_guard<std::mutex> lock(logMutex());
    std::FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "[%s] %.*s\n", levelTag(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(out);
    // Mirror into the app's sink (the editor's Output Log) after the write, under the same lock so the
    // sink sees lines in order. The sink must not re-enter logging (see setLogSink's contract).
    if (g_sink) g_sink(g_sinkCtx, level, message);
}

} // namespace detail
} // namespace aver
