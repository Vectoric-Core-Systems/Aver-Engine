#pragma once
// Aver.Core -- crash reporting.
//
// WHAT THIS IS FOR, stated first because it determines every design decision below: capturing enough
// about a crash, FROM INSIDE THE CRASHED PROCESS, that the cause can be found afterwards without a
// debugger attached and without asking the user to reproduce it.
//
// THE REPORT IS DISPLAYED BY A SEPARATE PROCESS, AverCrashReporter.exe, and that is the whole point
// rather than an implementation detail. A crash handler that tries to put a window on screen inside
// the process that just faulted is asking a broken process to run a message pump, allocate, and talk
// to the GPU driver -- the three things most likely to be exactly what broke. Every serious engine
// splits these for the same reason (Unreal's CrashReportClient, Chrome's crashpad_handler), and this
// follows Unreal's shape closely enough that its layout will look familiar: a per-crash folder
// holding a `CrashContext.runtime-xml`, a minidump, and the tail of the log.
//
// THE SEVERITY LADDER THIS SITS UNDER (see Log.hpp):
//   Error     -- something failed; the process is fine.
//   Critical  -- something failed in a way that threatens stability. The process is still running and
//                may well survive, but it is now a candidate to die. A Critical WAKES THE REPORTER:
//                AverCrashReporter.exe is launched in standby, watching this process, so that if the
//                death does follow there is already a live process to notice it -- one that did not
//                have to be spawned by a process in the middle of dying.
//   Fatal     -- death is imminent and this line is the last useful thing that will be said. Writing
//                a Fatal report and terminating is the only correct response; there is deliberately
//                no way to log Fatal and carry on.
//
// WHAT IT DOES NOT DO: no network, no upload, no telemetry, no phoning home. A report is written to
// disk on this machine and shown to the person sitting at it. Anything else is a policy decision that
// belongs to whoever ships a build, not to the engine.

#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::crash {

// Why the process is reporting. Written into the report as UE writes its own CrashType, because the
// first question anyone asks of a crash folder is "what kind of death was this".
//
// THE VALUES ARE EXPLICIT AND FROZEN, and the reason is the same one scripting_abi.h gives for the
// log levels: this number is written into a crash report as `CrashTypeCode` and read back by
// whatever triages the folder, so it outlives the build that produced it. A kind inserted in the
// middle silently renumbers every report ever written. APPEND, NEVER INSERT.
enum class Kind : int {
    Crash     = 0,  // an unhandled structured exception -- access violation, divide by zero, stack overflow
    Assert    = 1,  // AVER_ASSERT tripped: a programmer-invariant violation, not a runtime failure
    Fatal     = 2,  // an explicit AVER_FATAL: the code knew it could not continue
    GpuCrash  = 3,  // device removed / TDR. Distinct because the fix lives somewhere entirely different
    Terminate = 4,  // std::terminate: an unhandled C++ exception or a noexcept violation

    // ---- appended ------------------------------------------------------------------------------
    //
    // OutOfMemory is not a Crash even though it usually arrives as one. An allocation failure has a
    // completely different first question -- what was the working set, and what asked for how much --
    // and triaging it as an access violation sends the reader looking for a dangling pointer that
    // does not exist. The engine already distinguishes the case: AbiError::AllocationFailed and
    // LogLevel::Critical's own comment name "an allocation the engine needed and did not get".
    // ARRIVES TWO WAYS, and both are wired: STATUS_NO_MEMORY (0xC0000017) through the structured
    // exception filter, and a std::bad_alloc that escapes to std::terminate. There is deliberately no
    // third, speculative raiser -- a kind nothing can produce is a word in a vocabulary nobody speaks.
    OutOfMemory = 5,
};

// NOT HERE, and each for a reason worth stating, because "add a kind" is the obvious next thought:
//
//   A SHADER COMPILE FAILURE is not a crash in this engine. createShader logs AVER_ERROR and returns
//   0; the caller degrades. Nothing dies, so there is no report to file a kind on.
//
//   DEVICE LOSS is a Critical, not a crash: noteDeviceRemoved logs one and the process keeps running
//   with the last frame on screen. That Critical wakes the reporter to WATCH -- which is what
//   GpuCrash is for, if the process then dies -- rather than to file a report of its own.

const char* kindName(Kind k);

// The numeric code written into the report. A free function rather than a cast at each call site so
// that "what number is this kind" has exactly one answer, and so a reader grepping for the name
// finds the contract beside it.
inline int kindCode(Kind k) { return static_cast<int>(k); }

// The name for a raw code read back OUT of a report, which may have been written by a newer build
// than the one reading it. Never switches over the enum, for the reason abiErrorNameOf gives.
const char* kindNameOf(int code);

// Everything the report wants to know about the process that is not discoverable from inside a crash
// handler. Filled at startup, when allocating and formatting are still safe -- a crash handler must
// never be the first place a string is built.
struct Config {
    std::string appName       = "Aver";
    std::string engineVersion;      // Version.hpp's AVER_ENGINE_VERSION
    std::string buildConfig;        // "Debug" / "Release"
    std::string commandLine;
    std::string projectPath;        // may be empty; the editor runs without a project
    std::string gpuName;            // usually unknown at install time -- see setGpuName

    // Where crash folders are written. Empty means `<directory of this executable>/Saved/Crashes`,
    // which mirrors where Unreal puts them relative to a build.
    std::string crashDir;

    // AverCrashReporter.exe. Empty means "beside this executable", which is where the build puts it.
    std::string reporterPath;

    // OFF for headless test runs and for the gate harness. A suite that deliberately provokes a
    // failure must not leave a GUI process behind on the build machine.
    bool launchReporter = true;
};

// Installs the OS-level handlers: unhandled structured exceptions, std::terminate, pure-call and
// invalid-parameter. Idempotent -- a second call replaces the stored Config and returns.
//
// CALL THIS AS EARLY AS POSSIBLE. Anything that faults before install() produces the operating
// system's own dialog and no report at all, so the correct site is the first statement of main().
void install(const Config& cfg);

// Restores the previous handlers. The clean-exit path calls this so that a normal shutdown does not
// leave the standby reporter believing the process died.
void shutdown();

bool installed();

// Late-arriving context. The GPU name is not known until a device exists, and the project path
// changes when a project is opened -- both are read by the NEXT report, not baked in at install.
void setGpuName(std::string name);
void setProjectPath(std::string path);

// Turns the reporter launch on or off after install(). Exists for runs with nobody at the keyboard:
// an automated test that deliberately provokes a crash must not leave a GUI process waiting to be
// closed on a build machine.
void setLaunchReporter(bool on);

// Whether a debugger is attached right now.
//
// Exists so that Assert.cpp can ask WITHOUT including <windows.h> -- this translation unit already
// does. The answer decides whether a failed assert should break into the debugger (useful) or go
// straight to writing a report (the only option when nobody is attached). Breaking unconditionally
// raises EXCEPTION_BREAKPOINT, which with no debugger present is caught by our own unhandled filter
// and misfiled as a generic crash, losing the assert message and its file and line.
bool debuggerAttached();

// Records one formatted line into the in-memory ring the report's log tail is built from. Called by
// Log.cpp for every line, under the lock it already holds.
//
// THE ENGINE KEEPS NO LOG FILE -- Log.cpp's only sinks are stdio and one registered callback (the
// editor's Output Log panel), and neither survives the process. Without this ring a crash report
// would carry a stack and no narrative, and the narrative is usually what identifies the cause.
// Fixed slots, committed at startup, so recording a line cannot allocate or fail.
void noteLogLine(int level, std::string_view message);

// Called by the logger when a Critical is logged. The FIRST one launches the reporter into standby
// (see the header comment); later ones only append to the breadcrumb list the report carries, so a
// storm of criticals costs one process launch, not thousands.
void noteCritical(u32 code, std::string_view message);

// Writes a complete report folder and returns its absolute path, or an empty string if it could not
// be written. `exceptionPointers` is a Windows EXCEPTION_POINTERS* when one is available and null
// otherwise; it is typed void* so this header does not drag <windows.h> into everything that logs.
//
// Safe to call from a crashed state: it allocates nothing it can avoid, and every step is
// independently guarded so that failing to write the minidump still leaves the context file behind.
std::string writeReport(Kind kind, std::string_view message, void* exceptionPointers);

// Write a report, hand it to the reporter, and terminate the process. Never returns.
//
// There is no variant that returns. "Fatal" that execution continues past is the single most
// dangerous shape a logging API can have: it invites callers to treat the worst severity as a
// louder warning, and then the crash happens somewhere else with no report.
[[noreturn]] void fatal(Kind kind, std::string_view message);

}  // namespace aver::crash
