// What a failed AVER_ASSERT does.
#include "aver/core/Assert.hpp"
#include "aver/core/CrashReport.hpp"
#include "aver/core/Log.hpp"

#include <cstdlib>
#include <string>

namespace aver::detail {

// Logs the failed expression and its site, breaks into the debugger, then writes a crash report and
// terminates.
//
// CRITICAL, NOT ERROR, for the log line: a tripped assert is a programmer-invariant violation and the
// process is about to stop. Logging it at Error put the single most serious thing the engine can say
// on the same footing as a texture that failed to load.
//
// AVER_DEBUGBREAK STAYS, AND STAYS FIRST. Under a debugger it is far more useful than any report --
// it stops on the failing frame with every local still live. The report below is what happens when
// nobody is attached, which is every run that matters to a user.
void assertFail(const char* expr, const char* file, int line, std::string_view msg) {
    std::string detail;
    if (msg.empty()) {
        detail = std::string("ASSERT FAILED: (") + expr + ") at " + file + ":" + std::to_string(line);
    } else {
        detail = std::string("ASSERT FAILED: (") + expr + ") at " + file + ":" + std::to_string(line) +
                 " -- " + std::string(msg);
    }
    AVER_CRITICAL("{}", detail);
    // GUARDED, and it has to be. AVER_DEBUGBREAK with no debugger attached raises
    // EXCEPTION_BREAKPOINT (0x80000003), which our own unhandled-exception filter then catches and
    // files as a generic "Crash" -- throwing away the assert's expression, file and line, which are
    // the only things that made it an assert rather than an anonymous fault. Measured exactly that
    // way: --crash-test assert reported `unhandled exception (0x80000003)` until this guard existed.
    // With a debugger attached, breaking is still far better than any report.
    if (crash::debuggerAttached()) AVER_DEBUGBREAK();
    // crash::fatal, not std::abort: abort runs atexit handlers and static destructors on a process
    // that has already declared an invariant broken, and one of those faulting would replace a
    // precise assert report with a confusing secondary crash. It writes the report, hands it to the
    // reporter, and terminates without unwinding.
    crash::fatal(crash::Kind::Assert, detail);
}

} // namespace aver::detail
