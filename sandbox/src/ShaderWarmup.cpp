#include "ShaderWarmup.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
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
    bool done = false;
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

// Main thread, once a frame. The warm-up is a silent cache pre-filler now: the editor's own pipelines build off
// the main thread with their own "Compiling shaders" notification, so this only logs its end.
void ShaderWarmup::poll() {
#ifdef _WIN32
    if (!impl_->process || impl_->done) return;
    std::vector<std::string> got;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        got.swap(impl_->lines);
    }
    for (const std::string& l : got) {
        if (l.rfind("warm: done ", 0) == 0) {
            AVER_INFO("[ShaderWarm] shader cache filled in {:.0f} s", std::atof(l.c_str() + 11) / 1000.0);
            impl_->done = true;
        } else if (l.rfind("warm: fail", 0) == 0) {
            AVER_INFO("[ShaderWarm] {}", l);
        }
    }
    if (!impl_->done && impl_->exited) impl_->done = true;   // ended without "done": the log says why
#endif
}

}  // namespace aver::editor
