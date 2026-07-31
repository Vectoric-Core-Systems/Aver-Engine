#pragma once
// The engine log: four levels, a formatting front end, and the AVER_* macros.
#include "Prelude.hpp"
#include <format>
#include <string_view>
#include <utility>

namespace aver {

// Log severity, lowest first.
enum class LogLevel { Trace, Info, Warn, Error };

namespace detail {
void logWrite(LogLevel level, std::string_view message);
}

// A sink mirroring every log line into the application's own surface. Called under the log mutex,
// from the logging thread, so it must be quick and must not itself log.
using LogSinkFn = void (*)(void* ctx, LogLevel level, std::string_view message);
// Installs the sink, or removes it with {nullptr, nullptr}.
void setLogSink(LogSinkFn fn, void* ctx);

// Formats and writes one log line.
template <class... Args>
void logMsg(LogLevel level, std::string_view fmt, Args&&... args) {
    detail::logWrite(level, std::vformat(fmt, std::make_format_args(args...)));
}

} // namespace aver

#define AVER_TRACE(...) ::aver::logMsg(::aver::LogLevel::Trace, __VA_ARGS__)
#define AVER_INFO(...)  ::aver::logMsg(::aver::LogLevel::Info,  __VA_ARGS__)
#define AVER_WARN(...)  ::aver::logMsg(::aver::LogLevel::Warn,  __VA_ARGS__)
#define AVER_ERROR(...) ::aver::logMsg(::aver::LogLevel::Error, __VA_ARGS__)
