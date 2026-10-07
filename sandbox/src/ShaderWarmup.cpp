#include "ShaderWarmup.hpp"

#include "EditorNotifications.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aver::editor {

struct ShaderWarmup::Impl {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE readPipe = nullptr;
#endif
    std::thread reader;
    std::mutex mu;
    std::vector<std::string> lines;     // from the reader thread, drained by poll()
    std::atomic<bool> exited{false};
    u64 toast = 0;
    bool done = false;
    std::chrono::steady_clock::time_point t0{};
    std::string lastNote;

    void readLoop() {
#ifdef _WIN32
        std::string buf;
        char chunk[512];
        DWORD n = 0;
        while (ReadFile(readPipe, chunk, sizeof chunk, &n, nullptr) && n > 0) {
            buf.append(chunk, n);
            for (usize nl; (nl = buf.find('\n')) != std::string::npos;) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("warm: ", 0) == 0) {
                    std::lock_guard<std::mutex> lk(mu);
                    lines.push_back(std::move(line));
                }
            }
        }
#endif
        exited = true;
    }

    void close() {
#ifdef _WIN32
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 1);
            CloseHandle(process);
            process = nullptr;
        }
#endif
        if (reader.joinable()) reader.join();
#ifdef _WIN32
        if (readPipe) { CloseHandle(readPipe); readPipe = nullptr; }
#endif
    }
};

ShaderWarmup::ShaderWarmup() : impl_(std::make_unique<Impl>()) {}
ShaderWarmup::~ShaderWarmup() { impl_->close(); }

bool ShaderWarmup::running() const {
#ifdef _WIN32
    return impl_->process != nullptr && !impl_->done;
#else
    return false;
#endif
}

void ShaderWarmup::start(const std::string& projectManifest) {
#ifdef _WIN32
    if (running()) return;
    impl_->close();
    impl_->done = false;
    impl_->exited = false;
    impl_->toast = 0;
    impl_->t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->lines.clear();
    }
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
    // This same executable in --warm-shaders mode: the driver's pipeline cache is per executable.
    const std::filesystem::path tool = self;
    const std::filesystem::path dir = tool.parent_path();

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + tool.wstring() + L"\" --warm-shaders";
    if (!projectManifest.empty()) cmd += L" --project \"" + std::filesystem::path(projectManifest).wstring() + L"\"";
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        AVER_WARN("[ShaderWarm] could not start the shader warm-up (error {})", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->readPipe = rd;
    impl_->reader = std::thread([this] { impl_->readLoop(); });
    AVER_INFO("[ShaderWarm] filling the shader caches in the background (--warm-shaders, idle priority)");
#else
    (void)projectManifest;
#endif
}

void ShaderWarmup::poll() {
#ifdef _WIN32
    if (!impl_->process || impl_->done) return;
    std::vector<std::string> got;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        got.swap(impl_->lines);
    }
    NotificationQueue& q = notifications();
    // Shown once there is something to wait for: a real compile, or a run past 3 s (the driver's own
    // pipeline compiles report nothing). A warm cache finishes in about a second and stays silent.
    const auto showToast = [&] {
        if (impl_->toast) return;
        Notification n;
        n.severity = NotifySeverity::Info;
        n.title = "Preparing shaders";
        n.body = "Compiling renderer shaders in the background, so switching between ReSTIR RT, "
                 "Path Tracing and the denoisers loads from the cache.";
        n.sticky = true;
        n.hasProgress = true;
        n.progress = -1.0f;
        n.dedupKey = "shader-warmup";
        impl_->toast = q.push(std::move(n));
    };
    for (const std::string& l : got) {
        if (l.rfind("warm: shaders ", 0) == 0) {
            // "warm: shaders <requests> <compiled>": the toast appears with the first real compile.
            const std::string rest = l.substr(14);
            const usize sp = rest.find(' ');
            const long long compiled = sp == std::string::npos ? 0 : std::atoll(rest.c_str() + sp + 1);
            if (compiled > 0) showToast();
            impl_->lastNote = std::to_string(compiled) + " compiled so far";
            if (impl_->toast) q.setProgress(impl_->toast, -1.0f, impl_->lastNote);
        } else if (l.rfind("warm: done ", 0) == 0) {
            const double sec = std::atof(l.c_str() + 11) / 1000.0;
            char body[96];
            std::snprintf(body, sizeof body, "Shader cache ready (%.0f s, %s).", sec,
                          impl_->lastNote.empty() ? "pipelines built" : impl_->lastNote.c_str());
            if (impl_->toast) {
                q.setSticky(impl_->toast, false);   // finish() leaves sticky set; it must fade
                q.finish(impl_->toast, NotifySeverity::Success, "Shaders ready", body, 3.0);
            }
            AVER_INFO("[ShaderWarm] {}", body);
            impl_->done = true;
        } else if (l.rfind("warm: fail", 0) == 0) {
            AVER_INFO("[ShaderWarm] {}", l);
        }
    }
    if (!impl_->done && !impl_->exited &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->t0).count() > 3.0)
        showToast();
    if (!impl_->done && impl_->exited) {
        // Ended without "done" (failed, or no capable device): nothing to tell the user beyond the log.
        if (impl_->toast) q.close(impl_->toast);
        impl_->done = true;
    }
#endif
}

}  // namespace aver::editor
