// AverCrashReporter -- the separate process that shows a crash report.
//
// WHY THIS IS ITS OWN EXECUTABLE, and why it links NOTHING from the engine -- not Aver.Core, not the
// RHI, not a single header: it has to work when the engine does not. A reporter that shares a binary
// with the thing that crashed shares its heap, its static initialisers, its loaded drivers and its
// bugs. The one job here is to still be running and still be sane after the engine is gone, and the
// only way to be sure of that is to have nothing in common with it. Win32 and the CRT, nothing else.
//
// This is the shape Unreal uses (CrashReportClient is a separate program that reads a crash folder),
// and the folder it reads is deliberately Unreal-shaped too:
//
//     Saved/Crashes/AverCrash-<timestamp>-<pid>/
//         CrashContext.runtime-xml   the structured context: type, message, versions, callstack
//         AverMinidump.dmp           for a debugger, later
//         CrashLog.log               the tail of the engine log leading up to the fault
//
// TWO MODES:
//   --report <dir>   a crash has already happened; read that folder and show it.
//   --watch <pid>    STANDBY. A Critical was logged in that process. Wait for it. If it exits with a
//                    non-zero code and left a report folder behind, show the newest one. If it exits
//                    cleanly, say nothing and go away. This is what "a Critical wakes the reporter"
//                    means: the process that may be about to die spawns its own witness while it is
//                    still healthy enough to spawn anything.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

// ------------------------------------------------------------------------------------- utilities

std::string readWholeFile(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (32 << 20)) {
        CloseHandle(h);
        return {};
    }
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return {};
    out.resize(read);
    return out;
}

// A deliberately minimal XML value reader: find <tag> and take everything up to </tag>.
//
// NOT a parser, and that is the right call here. This reads exactly one file, written by exactly one
// writer (CrashReport.cpp), whose shape is fixed. Pulling in a real XML library would add a
// dependency to the one program in the tree whose entire value proposition is having none.
std::string tagValue(const std::string& xml, const char* tag) {
    const std::string open  = std::string("<")  + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return {};
    const size_t b = xml.find(close, a + open.size());
    if (b == std::string::npos) return {};
    return xml.substr(a + open.size(), b - a - open.size());
}

// Undo the writer's escaping. Same reasoning as tagValue: five entities, one writer, no library.
std::string unescape(std::string s) {
    struct { const char* from; char to; } table[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'},
    };
    for (auto& e : table) {
        const size_t n = std::strlen(e.from);
        for (size_t p = s.find(e.from); p != std::string::npos; p = s.find(e.from, p + 1))
            s.replace(p, n, 1, e.to);
    }
    return s;
}

// Windows edit controls want CRLF. A report full of bare newlines renders as one unreadable line.
std::string toCrlf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

std::string newestCrashDir(const std::string& root) {
    std::string best;
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((root + "\\AverCrash-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // The folder name starts with a sortable timestamp, so lexicographic max IS newest. That is
        // why crashId() formats the time the way it does -- see CrashReport.cpp.
        if (best.empty() || std::string(fd.cFileName) > best) best = fd.cFileName;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return best.empty() ? std::string() : root + "\\" + best;
}

// ------------------------------------------------------------------------------------- the report

struct Report {
    std::string dir;
    std::string type, message, when, app, version, config, gpu, project, cmdline, stack, breadcrumbs;
    std::string log;
    bool        valid = false;
};

Report loadReport(const std::string& dir) {
    Report r;
    r.dir = dir;
    const std::string xml = readWholeFile(dir + "\\CrashContext.runtime-xml");
    if (xml.empty()) return r;

    r.type    = unescape(tagValue(xml, "CrashType"));
    r.message = unescape(tagValue(xml, "ErrorMessage"));
    r.when    = unescape(tagValue(xml, "TimeOfCrash"));
    r.app     = unescape(tagValue(xml, "AppName"));
    r.version = unescape(tagValue(xml, "EngineVersion"));
    r.config  = unescape(tagValue(xml, "BuildConfiguration"));
    r.gpu     = unescape(tagValue(xml, "GPUBrand"));
    r.project = unescape(tagValue(xml, "ProjectPath"));
    r.cmdline = unescape(tagValue(xml, "CommandLine"));
    r.stack   = unescape(tagValue(xml, "CallStack"));

    // Breadcrumbs are repeated <Critical> elements, so tagValue's single-match shape does not fit.
    const std::string crumbs = tagValue(xml, "Breadcrumbs");
    for (size_t p = crumbs.find("<Critical>"); p != std::string::npos; p = crumbs.find("<Critical>", p + 1)) {
        const size_t e = crumbs.find("</Critical>", p);
        if (e == std::string::npos) break;
        r.breadcrumbs += "  " + unescape(crumbs.substr(p + 10, e - p - 10)) + "\n";
    }

    r.log   = readWholeFile(dir + "\\CrashLog.log");
    r.valid = true;
    return r;
}

std::string formatReport(const Report& r) {
    std::string s;
    s += r.app + " stopped unexpectedly.\n\n";
    s += "  What happened : " + r.type + " -- " + r.message + "\n";
    s += "  When          : " + r.when + "\n";
    s += "  Version       : " + r.version + (r.config.empty() ? "" : " (" + r.config + ")") + "\n";
    if (!r.gpu.empty())     s += "  GPU           : " + r.gpu + "\n";
    if (!r.project.empty()) s += "  Project       : " + r.project + "\n";
    if (!r.cmdline.empty()) s += "  Command line  : " + r.cmdline + "\n";
    s += "  Report folder : " + r.dir + "\n";

    if (!r.breadcrumbs.empty())
        s += "\nCritical errors logged before the crash:\n" + r.breadcrumbs;

    s += "\n--- Call stack ---\n";
    s += r.stack.empty() ? "(none captured)\n" : r.stack;

    if (!r.log.empty()) {
        s += "\n--- Log tail ---\n";
        // Bounded: the ring is 256 lines, but a defensive cap keeps a corrupt file from filling the
        // edit control with megabytes of noise.
        const size_t cap = 64 * 1024;
        s += r.log.size() > cap ? r.log.substr(r.log.size() - cap) : r.log;
    }
    return s;
}

// ------------------------------------------------------------------------------------- the window

constexpr int kIdText  = 1001;
constexpr int kIdCopy  = 1002;
constexpr int kIdOpen  = 1003;
constexpr int kIdClose = 1004;

const Report* g_report = nullptr;

void layout(HWND hwnd) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int pad = 10, btnH = 28, btnW = 150;
    const int textH = rc.bottom - pad * 3 - btnH;
    SetWindowPos(GetDlgItem(hwnd, kIdText), nullptr, pad, pad, rc.right - pad * 2, textH,
                 SWP_NOZORDER);
    const int y = rc.bottom - pad - btnH;
    SetWindowPos(GetDlgItem(hwnd, kIdCopy), nullptr, pad, y, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kIdOpen), nullptr, pad * 2 + btnW, y, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kIdClose), nullptr, rc.right - pad - btnW, y, btnW, btnH, SWP_NOZORDER);
}

void copyToClipboard(HWND hwnd, const std::string& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (h) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, text.c_str(), text.size() + 1);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
    }
    CloseClipboard();
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            layout(hwnd);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdCopy:
                    if (g_report) copyToClipboard(hwnd, formatReport(*g_report));
                    return 0;
                case kIdOpen:
                    if (g_report) ShellExecuteA(hwnd, "open", g_report->dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdClose:
                    DestroyWindow(hwnd);
                    return 0;
                default: break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int showWindow(const Report& r) {
    g_report = &r;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    // MAKEINTRESOURCEA(32512), not IDC_ARROW: the IDC_* macros resolve to the WIDE form under
    // a UNICODE build, and this program deliberately uses the A-suffixed Win32 API throughout
    // so that every path and message it handles is plain char, the same as the files it reads.
    wc.hCursor       = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AverCrashReporter";
    RegisterClassA(&wc);

    const std::string title = r.app + " crash report";
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, title.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    const std::string body = toCrlf(formatReport(r));
    HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", body.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdText), wc.hInstance, nullptr);

    // A fixed-pitch font, because a call stack in a proportional face is far harder to read and this
    // window exists to be read once, quickly, by someone who is already having a bad time.
    HFONT mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Consolas");
    SendMessageA(edit, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

    CreateWindowExA(0, "BUTTON", "Copy to clipboard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdCopy), wc.hInstance, nullptr);
    CreateWindowExA(0, "BUTTON", "Open report folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdOpen), wc.hInstance, nullptr);
    CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdClose), wc.hInstance, nullptr);

    layout(hwnd);
    MessageBeep(MB_ICONERROR);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    DeleteObject(mono);
    return 0;
}

// ------------------------------------------------------------------------------------ standby mode

int watchProcess(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;   // already gone, and we were not there to see how -- nothing to report

    // Snapshot BEFORE waiting, so "a folder appeared while we watched" is distinguishable from "a
    // folder was already lying there from a crash last Tuesday".
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (char* slash = std::strrchr(exePath, '\\')) *slash = 0;
    const std::string crashRoot = std::string(exePath) + "\\Saved\\Crashes";
    const std::string before    = newestCrashDir(crashRoot);

    WaitForSingleObject(h, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(h, &exitCode);
    CloseHandle(h);

    if (exitCode == 0) return 0;   // clean exit after a Critical: it survived. Say nothing.

    const std::string after = newestCrashDir(crashRoot);
    if (after.empty() || after == before) {
        // It died without writing a report -- killed, or faulted somewhere the handler could not
        // reach (a stack overflow in the handler itself, a TerminateProcess from outside). Still
        // worth saying so: a silent disappearance is the worst possible outcome for the user.
        Report r;
        r.valid   = true;
        r.app     = "Aver";
        r.dir     = crashRoot;
        r.type    = "Unreported";
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "the process exited with code %lu (0x%08lX) after logging a critical error, "
                      "and left no crash report behind", exitCode, exitCode);
        r.message = buf;
        return showWindow(r);
    }

    const Report r = loadReport(after);
    if (!r.valid) return 0;
    return showWindow(r);
}

}  // namespace

// Writes the formatted report to stdout and exits, with no window at all.
//
// This exists for two callers that both matter: an automated check that wants to assert on the
// CONTENT of a report without a GUI process being left behind on a build machine, and a person
// triaging a crash folder over a terminal or an SSH session where no window can be shown. A crash
// reporter that can only communicate through a window is unusable in exactly the situations where
// crashes are least convenient.
//
// Note the /SUBSYSTEM:WINDOWS link option means there is no console attached by default, so this
// reattaches to the parent's if there is one -- otherwise printf goes nowhere and the mode looks
// broken rather than silent.
int printReport(const Report& r) {
    // ONLY touch the console if stdout is not already going somewhere. A GUI-subsystem process
    // launched with its output redirected to a pipe or a file inherits a perfectly good
    // STD_OUTPUT_HANDLE, and reopening CONOUT$ over the top of it would send the report to a console
    // instead of to the pipe the caller is reading -- which looks exactly like the mode not working.
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing == nullptr || existing == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }
    const std::string body = formatReport(r);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    std::string dir;
    DWORD watchPid = 0;
    bool  print    = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--report") && i + 1 < argc)      dir = argv[++i];
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc)  watchPid = std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--print"))                  print = true;
    }

    if (watchPid) return watchProcess(watchPid);

    if (dir.empty()) {
        // No arguments: show the most recent crash. Makes the reporter useful on its own, which is
        // what someone double-clicking it in a build folder is trying to do.
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (char* slash = std::strrchr(exePath, '\\')) *slash = 0;
        dir = newestCrashDir(std::string(exePath) + "\\Saved\\Crashes");
    }
    if (dir.empty()) {
        if (print) { std::printf("No crash reports found.\n"); return 0; }
        MessageBoxA(nullptr, "No crash reports found.", "Aver Crash Reporter", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const Report r = loadReport(dir);
    if (!r.valid) {
        if (print) { std::printf("Could not read a crash report from: %s\n", dir.c_str()); return 1; }
        MessageBoxA(nullptr, ("Could not read a crash report from:\n" + dir).c_str(),
                    "Aver Crash Reporter", MB_OK | MB_ICONWARNING);
        return 1;
    }
    return print ? printReport(r) : showWindow(r);
}
