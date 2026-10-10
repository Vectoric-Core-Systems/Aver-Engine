// The log's stdio writer and the optional application sink.
#include "aver/core/Log.hpp"
#include "aver/core/CrashReport.hpp"

#include <cstdio>
#include <mutex>

namespace aver {

namespace {
// The one mutex guarding both the stdio write and the sink pointer.
std::mutex& logMutex() { static std::mutex m; return m; }
LogSinkFn   g_sink    = nullptr;
void*       g_sinkCtx = nullptr;
std::FILE*  g_file    = nullptr;
} // namespace

bool setLogFile(const char* path) {
    std::lock_guard<std::mutex> lock(logMutex());
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    if (!path) return true;
    g_file = std::fopen(path, "w");
    return g_file != nullptr;
}

// Installs or removes the application's log sink.
void setLogSink(LogSinkFn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(logMutex());
    g_sink    = fn;
    g_sinkCtx = ctx;
}

namespace detail {

// The five-character tag printed for a level.
//
// FIVE CHARACTERS EACH, PADDED, and that is not cosmetic: tools/mcp/aver_mcp.py scrapes stdout for
// the bracketed tag, and the gate summary's error and warning lists are built from
// `"[ERROR" in line` / `"[WARN" in line` style matches. "FATAL" in particular is constrained -- that
// file has carried a dormant `"[FATAL" in l` match since long before this level existed, so the tag
// has to be exactly that spelling for it to finally fire. "CRIT " is matched there as "[CRIT".
// Renaming either of these is a silent break of the MCP gate summaries, not a compile error.
static const char* levelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Info:     return "INFO ";
        case LogLevel::Warn:     return "WARN ";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT ";
        case LogLevel::Fatal:    return "FATAL";
    }
    return "?????";
}

// Writes one formatted line to stdio and then to the sink, both under the log mutex.
void logWrite(LogLevel level, std::string_view message) {
    // Recorded FIRST, before any routing decision, so that the tail a crash report carries includes
    // the line that is about to kill the process rather than stopping one short of it.
    crash::noteLogLine(static_cast<int>(level), message);

    {
        std::lock_guard<std::mutex> lock(logMutex());
        // A THRESHOLD, not the two-value OR-chain this used to be. `level == Error || level == Warn`
        // silently routed the two levels added above it to stdout -- so a Fatal, the single most
        // important line the process will ever print, would not have gone to stderr at all.
        std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
        std::fprintf(out, "[%s] %.*s\n", levelTag(level),
                     static_cast<int>(message.size()), message.data());
        std::fflush(out);
        if (g_file) {
            std::fprintf(g_file, "[%s] %.*s\n", levelTag(level), static_cast<int>(message.size()), message.data());
            std::fflush(g_file);
        }
        if (g_sink) g_sink(g_sinkCtx, level, message);
    }

    // OUTSIDE the lock. noteCritical may launch the reporter process, and CreateProcess is far too
    // slow to hold a mutex every logging thread wants across. It is also re-entrant-safe only
    // because it does not log.
    if (level == LogLevel::Critical) crash::noteCritical(0, message);
}

[[noreturn]] void logFatal(std::string_view message) {
    logWrite(LogLevel::Fatal, message);
    crash::fatal(crash::Kind::Fatal, message);
}

} // namespace detail
} // namespace aver
