#pragma once
// The engine log: six levels, a formatting front end, and the AVER_* macros.
#include "Prelude.hpp"
#include <format>
#include <string_view>
#include <utility>

namespace aver {

// Log severity, lowest first.
//
// THE ORDER IS LOAD-BEARING and the two top levels were APPENDED, never inserted. The editor's
// filter compares `(int)level < minLevel`, the scripting ABI mirrors these as bare integers
// (scripting_abi.h), and C# mirrors them again (Aver.Scripting/Log.cs) -- inserting a value in the
// middle would silently renumber every one of those without a single compile error.
//
// WHERE THE LINE IS between the top three, because "how bad is this" is otherwise a matter of taste
// and the ladder stops meaning anything within a week:
//
//   Error    -- an operation failed. The process is healthy and the next frame will be fine.
//               A failed file load, a shader that would not compile, a bad argument.
//
//   Critical -- something failed in a way that threatens the PROCESS, not just the operation. It is
//               still running, and it may well survive, but it is now a candidate to die: a lost GPU
//               device, an exhausted descriptor heap, an allocation the engine needed and did not
//               get. Logging one WAKES THE CRASH REPORTER (CrashReport.hpp) -- a separate process is
//               launched to watch this one, while this one is still healthy enough to launch it.
//
//   Fatal    -- death is imminent and this is the last useful thing that will be said. AVER_FATAL
//               does not return: it writes a crash report and terminates. There is deliberately no
//               way to log Fatal and carry on, because a "fatal" that execution continues past is
//               how the worst severity quietly becomes a louder warning.
enum class LogLevel { Trace, Info, Warn, Error, Critical, Fatal };

namespace detail {
void logWrite(LogLevel level, std::string_view message);
[[noreturn]] void logFatal(std::string_view message);
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

// Formats, writes, reports and terminates. Separate from logMsg because it is [[noreturn]] and the
// compiler should know it: code after an AVER_FATAL is unreachable, and saying so lets the optimiser
// and every "not all control paths return a value" diagnostic agree with the programmer.
template <class... Args>
[[noreturn]] void logMsgFatal(std::string_view fmt, Args&&... args) {
    detail::logFatal(std::vformat(fmt, std::make_format_args(args...)));
}

} // namespace aver

#define AVER_TRACE(...)    ::aver::logMsg(::aver::LogLevel::Trace,    __VA_ARGS__)
#define AVER_INFO(...)     ::aver::logMsg(::aver::LogLevel::Info,     __VA_ARGS__)
#define AVER_WARN(...)     ::aver::logMsg(::aver::LogLevel::Warn,     __VA_ARGS__)
#define AVER_ERROR(...)    ::aver::logMsg(::aver::LogLevel::Error,    __VA_ARGS__)
#define AVER_CRITICAL(...) ::aver::logMsg(::aver::LogLevel::Critical, __VA_ARGS__)
// Writes a crash report and terminates. Never returns.
#define AVER_FATAL(...)    ::aver::logMsgFatal(__VA_ARGS__)
