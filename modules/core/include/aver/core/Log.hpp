#pragma once
#include "Prelude.hpp"
#include <format>
#include <string_view>
#include <utility>

namespace aver {

enum class LogLevel { Trace, Info, Warn, Error };

namespace detail {
void logWrite(LogLevel level, std::string_view message);
}

// A sink the application can install to mirror EVERY log line into its own surface (the editor's Output
// Log). It is called under the log mutex, from whatever thread logged, so it must be quick and must NOT
// itself log (that would re-enter the held mutex and deadlock). Installing {nullptr, nullptr} removes it.
using LogSinkFn = void (*)(void* ctx, LogLevel level, std::string_view message);
void setLogSink(LogSinkFn fn, void* ctx);

template <class... Args>
void logMsg(LogLevel level, std::string_view fmt, Args&&... args) {
    detail::logWrite(level, std::vformat(fmt, std::make_format_args(args...)));
}

} // namespace aver

#define AVER_TRACE(...) ::aver::logMsg(::aver::LogLevel::Trace, __VA_ARGS__)
#define AVER_INFO(...)  ::aver::logMsg(::aver::LogLevel::Info,  __VA_ARGS__)
#define AVER_WARN(...)  ::aver::logMsg(::aver::LogLevel::Warn,  __VA_ARGS__)
#define AVER_ERROR(...) ::aver::logMsg(::aver::LogLevel::Error, __VA_ARGS__)
