#include "aver/core/CrashReport.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
// Linked here rather than in CMake deliberately: dbghelp is a Windows SDK library used by exactly
// one translation unit in the whole engine, and a #pragma keeps that fact next to the code that
// depends on it instead of in a build file three directories away. It also means aver_add_module's
// argument list does not have to grow a platform-conditional branch for one file.
#pragma comment(lib, "dbghelp.lib")
#endif

namespace aver::crash {
namespace {

// ---------------------------------------------------------------------------------------- state
//
// DELIBERATELY FLAT AND PRE-ALLOCATED. Every std::string here is filled at install() time, when the
// process is healthy. A crash handler that has to build its configuration while unwinding a fault is
// one allocation away from a second, more confusing crash inside the reporter for the first.

std::mutex& stateMutex() {
    static std::mutex m;
    return m;
}

Config      g_cfg;
bool        g_installed        = false;
bool        g_reporterAwake    = false;   // has the standby reporter been launched yet
bool        g_reportInFlight   = false;   // re-entrancy guard: a fault INSIDE writeReport
char        g_exeDir[MAX_PATH] = {};

#if defined(_WIN32)
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
#endif

// Criticals seen so far, as breadcrumbs in the report. Bounded: a storm must not become the report.
constexpr int kMaxBreadcrumbs = 32;
std::vector<std::string> g_breadcrumbs;

// ------------------------------------------------------------------------------------- log ring
//
// The last N formatted log lines, so a report can carry the tail of the log without the engine
// having to keep a log FILE (it does not -- see Log.cpp, whose only sinks are stdio and one
// registered callback). A fixed ring of fixed-width slots: no allocation per line, no growth, and
// the memory is committed at startup so a crash cannot fail to write into it.
constexpr int kRingLines = 256;
constexpr int kRingWidth = 512;

char  g_ring[kRingLines][kRingWidth] = {};
int   g_ringNext                     = 0;   // next slot to write
bool  g_ringWrapped                  = false;

}  // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Crash:         return "Crash";
        case Kind::Assert:        return "Assert";
        case Kind::Fatal:         return "Fatal";
        case Kind::GpuCrash:      return "GPUCrash";
        case Kind::Terminate:     return "Terminate";
        case Kind::OutOfMemory:   return "OutOfMemory";
    }
    return "Unknown";
}

// A RAW int, for a report written by a build newer than the one reading it. Switching over the enum
// on such a value would be undefined behaviour; this names what it knows and admits the rest.
const char* kindNameOf(int code) {
    switch (code) {
        case 0: return "Crash";
        case 1: return "Assert";
        case 2: return "Fatal";
        case 3: return "GPUCrash";
        case 4: return "Terminate";
        case 5: return "OutOfMemory";
        default: return "Unknown";
    }
}

void noteLogLine(int level, std::string_view message) {
    std::lock_guard<std::mutex> lock(stateMutex());
    char* slot = g_ring[g_ringNext];
    const int n = std::snprintf(slot, kRingWidth, "[%d] %.*s", level,
                                static_cast<int>(message.size()), message.data());
    (void)n;
    g_ringNext = (g_ringNext + 1) % kRingLines;
    if (g_ringNext == 0) g_ringWrapped = true;
}

#if !defined(_WIN32)

// ------------------------------------------------------------------- non-Windows: honest no-op
//
// Stated rather than silently compiled away: this engine is Windows-only today (the RHI has a D3D12
// and a Vulkan backend, but the window, the entry point and the packaging are all Win32). A crash
// reporter is the most platform-specific thing in a codebase, and a half-ported one that writes an
// empty report is worse than one that says it is not implemented.
void install(const Config& cfg) { std::lock_guard<std::mutex> l(stateMutex()); g_cfg = cfg; g_installed = false; }
void shutdown() {}
bool installed() { return false; }
void setGpuName(std::string) {}
void setProjectPath(std::string) {}
void setLaunchReporter(bool) {}
bool debuggerAttached() { return false; }
void noteCritical(u32, std::string_view) {}
std::string writeReport(Kind, std::string_view, void*) { return {}; }
[[noreturn]] void fatal(Kind, std::string_view msg) {
    std::fprintf(stderr, "[FATAL] %.*s\n", static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
    std::abort();
}

#else

namespace {

// ------------------------------------------------------------------------------------- helpers

void exeDirectory(char* out, DWORD cap) {
    out[0] = 0;
    if (!GetModuleFileNameA(nullptr, out, cap)) return;
    char* slash = std::strrchr(out, '\\');
    if (slash) *slash = 0;
}

// CreateDirectory for every component. Returns false only if the leaf could not be made; an
// already-existing component is success, which is what ERROR_ALREADY_EXISTS means here.
bool makeDirectories(const char* path) {
    char buf[MAX_PATH];
    std::snprintf(buf, sizeof buf, "%s", path);
    for (char* p = buf + 3; *p; ++p) {     // +3 skips "C:\", which cannot be created
        if (*p != '\\' && *p != '/') continue;
        const char saved = *p;
        *p = 0;
        CreateDirectoryA(buf, nullptr);
        *p = saved;
    }
    return CreateDirectoryA(buf, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// A crash id that is unique without needing ole32/CoCreateGuid. Time to the millisecond plus the
// process id is unique in practice, sorts chronologically in a folder listing, and -- unlike a real
// GUID -- tells a human when the crash happened just by looking at the folder name.
void crashId(char* out, size_t cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(out, cap, "%04u.%02u.%02u-%02u.%02u.%02u.%03u-%lu",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, GetCurrentProcessId());
}

void isoTimestamp(char* out, size_t cap) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    std::snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// XML text escaping. A command line with an ampersand in it must not silently truncate the report
// when the reporter parses it -- and command lines routinely contain & and quotes.
void appendEscaped(std::string& dst, std::string_view src) {
    for (char c : src) {
        switch (c) {
            case '&':  dst += "&amp;";  break;
            case '<':  dst += "&lt;";   break;
            case '>':  dst += "&gt;";   break;
            case '"':  dst += "&quot;"; break;
            case '\'': dst += "&apos;"; break;
            default:
                // Strip control characters: they are illegal in XML 1.0 and turn a readable report
                // into an unparseable one for no gain.
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t') dst += c;
                break;
        }
    }
}

bool writeWholeFile(const char* path, const void* data, size_t bytes) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, static_cast<DWORD>(bytes), &written, nullptr);
    CloseHandle(h);
    return ok && written == bytes;
}

// ------------------------------------------------------------------------------- stack capture
//
// Symbolicated in-process, guarded. The minidump beside it is the authoritative artefact -- it can be
// opened in a debugger with full symbols long after the fact -- but a human reading the XML wants to
// see the top frames without opening anything, and that is worth the risk of a symbol lookup here.
// The whole walk sits under __try so that a corrupt stack produces a short report rather than a
// second fault inside the handler for the first.
// SPLIT IN TWO, and MSVC requires it rather than preferring it: C2712 forbids __try in any function
// that needs object unwinding, which means the guarded part cannot touch a std::string. So the walk
// writes into a fixed char buffer with nothing destructible in scope, and the caller wraps it.
void captureStackRaw(CONTEXT* ctx, char* out, size_t cap) {
    out[0] = 0;
    size_t used = 0;
    const HANDLE proc   = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    __try {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(proc, nullptr, TRUE);

        CONTEXT local;
        if (ctx) {
            local = *ctx;
        } else {
            RtlCaptureContext(&local);
        }

        STACKFRAME64 frame = {};
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset    = local.Rip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = local.Rbp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = local.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;

        char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 511;

        for (int depth = 0; depth < 64; ++depth) {
            if (!StackWalk64(machine, proc, thread, &frame, &local, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (frame.AddrPC.Offset == 0) break;

            char line[768];
            DWORD64 disp = 0;
            if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
                IMAGEHLP_LINE64 il = {};
                il.SizeOfStruct = sizeof(il);
                DWORD lineDisp = 0;
                if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &lineDisp, &il)) {
                    std::snprintf(line, sizeof line, "%s (%s:%lu)\n", sym->Name, il.FileName, il.LineNumber);
                } else {
                    std::snprintf(line, sizeof line, "%s + 0x%llx\n", sym->Name,
                                  static_cast<unsigned long long>(disp));
                }
            } else {
                std::snprintf(line, sizeof line, "0x%016llx\n",
                              static_cast<unsigned long long>(frame.AddrPC.Offset));
            }
            const size_t n = std::strlen(line);
            if (used + n + 1 >= cap) break;
            std::memcpy(out + used, line, n + 1);
            used += n;
        }
        SymCleanup(proc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const char* msg = "<stack walk faulted; see the minidump>\n";
        const size_t n = std::strlen(msg);
        if (used + n + 1 < cap) { std::memcpy(out + used, msg, n + 1); used += n; }
    }
    if (used == 0) std::snprintf(out, cap, "%s", "<no frames captured; see the minidump>\n");
}

std::string captureStack(CONTEXT* ctx) {
    // Heap, not a 32 KB stack array: this runs on a thread that has just faulted, and a stack
    // overflow is one of the exceptions it most needs to survive long enough to report.
    std::vector<char> buf(32 * 1024);
    captureStackRaw(ctx, buf.data(), buf.size());
    return std::string(buf.data());
}

std::string logTail() {
    std::string out;
    const int count = g_ringWrapped ? kRingLines : g_ringNext;
    const int start = g_ringWrapped ? g_ringNext : 0;
    for (int i = 0; i < count; ++i) {
        const char* s = g_ring[(start + i) % kRingLines];
        if (!s[0]) continue;
        out += s;
        out += '\n';
    }
    return out;
}

// ------------------------------------------------------------------------------------- minidump

bool writeMinidump(const char* path, EXCEPTION_POINTERS* ep) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    // WithIndirectlyReferencedMemory is what makes a dump actually useful without being enormous: it
    // pulls in the memory that stack values point AT, so locals and this-pointers can be inspected,
    // rather than only the raw stack words. WithDataSegs would add every global in every module and
    // multiply the file size for far less diagnostic value.
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules);

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type,
                                      ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// --------------------------------------------------------------------------- reporter launching

std::string reporterExecutable() {
    if (!g_cfg.reporterPath.empty()) return g_cfg.reporterPath;
    std::string p = g_exeDir;
    p += "\\AverCrashReporter.exe";
    return p;
}

// Launches the reporter. `args` is appended after the executable path.
//
// DETACHED, and that word is load-bearing: DETACHED_PROCESS plus no inherited handles means the
// reporter does not die when this process does, which is the entire reason it exists.
bool spawnReporter(const std::string& args) {
    const std::string exe = reporterExecutable();
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::string cmd = "\"" + exe + "\" " + args;
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                   DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ----------------------------------------------------------------------------- the OS handlers

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;

    char msg[256];
    const char* name = "unhandled exception";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      name = "access violation";            break;
        case EXCEPTION_STACK_OVERFLOW:        name = "stack overflow";              break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "integer divide by zero";      break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "float divide by zero";        break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "illegal instruction";         break;
        case EXCEPTION_PRIV_INSTRUCTION:      name = "privileged instruction";      break;
        case EXCEPTION_IN_PAGE_ERROR:         name = "in-page error";               break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: name = "datatype misalignment";       break;
        // STATUS_NO_MEMORY, named by value because winnt.h does not define it as an EXCEPTION_*.
        // A real arrival path for an exhausted address space, and reported as a generic Crash it
        // sends the reader hunting a dangling pointer that does not exist.
        case 0xC0000017L:                     name = "out of memory";               break;
        // MSVC's code for a C++ `throw`. THIS FILTER SEES AN ESCAPING EXCEPTION BEFORE
        // std::terminate does -- measured with --crash-test oom, which filed a Crash until this
        // existed -- so the Terminate kind has to be decided here, not only in terminateHandler.
        case 0xE06D7363L:                     name = "an unhandled C++ exception escaped";  break;
        default: break;
    }

    // An access violation says WHERE and WHICH WAY, and both matter: a null write and a wild read are
    // different bugs, and the report should not make someone open the dump to tell them apart.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR op   = ep->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
        std::snprintf(msg, sizeof msg, "%s (0x%08lX) %s address 0x%016llx", name, code,
                      op == 0 ? "reading" : op == 1 ? "writing" : "executing",
                      static_cast<unsigned long long>(addr));
    } else {
        std::snprintf(msg, sizeof msg, "%s (0x%08lX)", name, code);
    }

    // The KIND, not just the message, so a triage tool sorting on CrashTypeCode sees it too.
    const Kind kind = code == 0xC0000017L ? Kind::OutOfMemory
                    : code == 0xE06D7363L ? Kind::Terminate
                                          : Kind::Crash;
    writeReport(kind, msg, ep);

    // EXECUTE_HANDLER, not CONTINUE_SEARCH: the report is written, and letting the default handler
    // also run would put the OS "a program stopped working" dialog on top of our own reporter.
    return EXCEPTION_EXECUTE_HANDLER;
}

void terminateHandler() {
    // WHAT ESCAPED, when it is still knowable -- rethrowing inside a try recovers the in-flight
    // exception's type, so the report carries its what() rather than a generic sentence.
    //
    // THIS IS NOT THE MAIN PATH FOR AN ESCAPING EXCEPTION, and the comment says so because the
    // obvious reading is wrong: on Windows a `throw` that nobody catches raises SEH 0xE06D7363 and
    // unhandledFilter takes it first. What reaches here is terminate called DIRECTLY -- a noexcept
    // function that threw, a failed dynamic_cast on a reference in some configurations, an explicit
    // call -- and in several of those current_exception() is null, which the fall-through covers.
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::bad_alloc& e) {
            fatal(Kind::OutOfMemory, std::string("std::bad_alloc escaped: ") + e.what());
        } catch (const std::exception& e) {
            fatal(Kind::Terminate, std::string("an exception escaped: ") + e.what());
        } catch (...) {
            fatal(Kind::Terminate, "a non-std exception escaped");
        }
    }
    fatal(Kind::Terminate, "std::terminate called -- an exception escaped, or a noexcept function threw");
}

void purecallHandler() {
    fatal(Kind::Fatal, "pure virtual function called -- an object was used during construction or destruction");
}

void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    // Deliberately NOT forwarding the wide-char detail arguments: they are only populated in debug
    // CRT builds, and converting them here means allocating and calling into the CRT from inside a
    // CRT failure handler. The stack in the report says far more than the parameter name would.
    fatal(Kind::Fatal, "CRT invalid parameter -- a standard library function was called with arguments it rejects");
}

// EVERY FAILED ALLOCATION, and the only hook that reliably sees one. operator new calls the installed
// new-handler and retries until the handler succeeds, throws, or ends the process -- so this runs
// BEFORE std::bad_alloc is constructed, which matters twice over: the report is filed while the
// handler still knows the allocation is what failed, and it does not depend on the throw reaching
// anybody, which -- as `--crash-test oom` demonstrated -- it does not.
//
// WHY NOT std::terminate. That was the first design here, and it was wrong: on Windows an escaping
// C++ exception raises SEH code 0xE06D7363 and is taken by SetUnhandledExceptionFilter before
// std::terminate ever runs. The first `--crash-test oom` filed a `Crash`, not an `OutOfMemory`, and
// that is what sent this to the new-handler instead. terminateHandler still matters for the case it
// really does own -- a noexcept violation, or terminate called outright.
void newHandler() {
    fatal(Kind::OutOfMemory,
          "operator new could not satisfy an allocation -- the process is out of address space or "
          "the request was absurd. The working set and the requested size are the first questions, "
          "not the call stack.");
}

}  // namespace

// ------------------------------------------------------------------------------------- public API

void install(const Config& cfg) {
    std::lock_guard<std::mutex> lock(stateMutex());
    g_cfg = cfg;
    exeDirectory(g_exeDir, MAX_PATH);
    if (g_installed) return;

    g_prevFilter = SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateHandler);
    std::set_new_handler(newHandler);
    _set_purecall_handler(purecallHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);

    // The CRT's own "this application has requested the Runtime to terminate it in an unusual way"
    // dialog and the Windows Error Reporting dialog both race ours and both block on a human. Turned
    // off so the only window a user sees is the reporter.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    g_installed = true;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(stateMutex());
    if (!g_installed) return;
    SetUnhandledExceptionFilter(g_prevFilter);
    g_prevFilter = nullptr;
    g_installed  = false;
}

bool installed() {
    std::lock_guard<std::mutex> lock(stateMutex());
    return g_installed;
}

void setGpuName(std::string name) {
    std::lock_guard<std::mutex> lock(stateMutex());
    g_cfg.gpuName = std::move(name);
}

void setProjectPath(std::string path) {
    std::lock_guard<std::mutex> lock(stateMutex());
    g_cfg.projectPath = std::move(path);
}

void setLaunchReporter(bool on) {
    std::lock_guard<std::mutex> lock(stateMutex());
    g_cfg.launchReporter = on;
}

bool debuggerAttached() { return IsDebuggerPresent() != FALSE; }

void noteCritical(u32 code, std::string_view message) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex());
        if (g_breadcrumbs.size() < kMaxBreadcrumbs) {
            char buf[512];
            std::snprintf(buf, sizeof buf, "AVR%04u: %.*s", code,
                          static_cast<int>(message.size()), message.data());
            g_breadcrumbs.emplace_back(buf);
        }
        if (g_installed && g_cfg.launchReporter && !g_reporterAwake) {
            g_reporterAwake = true;
            wake = true;
        }
    }
    // Outside the lock: CreateProcess is slow enough that holding a mutex every logger thread may
    // want across it is a needless stall, and the flag above already guarantees one launch.
    if (wake) {
        char args[128];
        std::snprintf(args, sizeof args, "--watch %lu", GetCurrentProcessId());
        spawnReporter(args);
    }
}

std::string writeReport(Kind kind, std::string_view message, void* exceptionPointers) {
    // RE-ENTRANCY. If writing a report itself faults, the unhandled filter runs again and we would
    // recurse until the stack ran out -- turning a diagnosable crash into a stack overflow with no
    // report at all. One report per process is the right answer: the first is the interesting one.
    {
        std::lock_guard<std::mutex> lock(stateMutex());
        if (g_reportInFlight) return {};
        g_reportInFlight = true;
    }

    char id[128];
    crashId(id, sizeof id);

    std::string dir = g_cfg.crashDir;
    if (dir.empty()) {
        dir = g_exeDir;
        dir += "\\Saved\\Crashes";
    }
    dir += "\\AverCrash-";
    dir += id;

    if (!makeDirectories(dir.c_str())) {
        std::lock_guard<std::mutex> lock(stateMutex());
        g_reportInFlight = false;
        return {};
    }

    // The minidump FIRST, and deliberately: it is the artefact that depends most on the process still
    // being in the state that faulted, and every step after this one only reads memory we own.
    const std::string dumpPath = dir + "\\AverMinidump.dmp";
    const bool haveDump = writeMinidump(dumpPath.c_str(), static_cast<EXCEPTION_POINTERS*>(exceptionPointers));

    auto* ep = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    const std::string stack = captureStack(ep ? ep->ContextRecord : nullptr);

    char when[64];
    isoTimestamp(when, sizeof when);

    std::string xml;
    xml.reserve(8192);
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<AverCrashContext>\n  <RuntimeProperties>\n";
    auto tag = [&xml](const char* name, std::string_view value) {
        xml += "    <"; xml += name; xml += ">";
        appendEscaped(xml, value);
        xml += "</"; xml += name; xml += ">\n";
    };
    tag("CrashVersion",       "1");
    tag("CrashGUID",          id);
    tag("CrashType",          kindName(kind));
    // THE NUMBER BESIDE THE NAME. A name is what a person reads; a stable code is what anything
    // triaging a folder of reports can group by without string-matching a label that may be
    // reworded. See Kind's own comment for why these values are frozen.
    tag("CrashTypeCode",      std::to_string(kindCode(kind)));
    tag("ErrorMessage",       message);
    tag("TimeOfCrash",        when);
    tag("AppName",            g_cfg.appName);
    tag("EngineVersion",      g_cfg.engineVersion);
    tag("BuildConfiguration", g_cfg.buildConfig);
    tag("PlatformName",       "Windows");
    tag("GPUBrand",           g_cfg.gpuName);
    tag("ProjectPath",        g_cfg.projectPath);
    tag("CommandLine",        g_cfg.commandLine);
    tag("MinidumpPresent",    haveDump ? "true" : "false");

    char pid[32];
    std::snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
    tag("ProcessId", pid);

    xml += "    <Breadcrumbs>\n";
    {
        std::lock_guard<std::mutex> lock(stateMutex());
        for (const std::string& b : g_breadcrumbs) {
            xml += "      <Critical>";
            appendEscaped(xml, b);
            xml += "</Critical>\n";
        }
    }
    xml += "    </Breadcrumbs>\n";

    tag("CallStack", stack);
    xml += "  </RuntimeProperties>\n</AverCrashContext>\n";

    writeWholeFile((dir + "\\CrashContext.runtime-xml").c_str(), xml.data(), xml.size());

    const std::string tail = logTail();
    writeWholeFile((dir + "\\CrashLog.log").c_str(), tail.data(), tail.size());

    if (g_cfg.launchReporter) spawnReporter("--report \"" + dir + "\"");

    // NOT cleared. g_reportInFlight stays true for the life of the process on purpose -- see the
    // re-entrancy comment above. A process that has already produced one report is not in a state
    // where a second one would be more informative.
    return dir;
}

[[noreturn]] void fatal(Kind kind, std::string_view message) {
    writeReport(kind, message, nullptr);
    // TerminateProcess, not abort(): abort() runs atexit handlers and static destructors, and a
    // process that has just declared itself unable to continue should not then run arbitrary
    // shutdown code that may fault again and replace a good report with a confusing one.
    TerminateProcess(GetCurrentProcess(), 3);
    __assume(0);
}

#endif  // _WIN32

}  // namespace aver::crash
