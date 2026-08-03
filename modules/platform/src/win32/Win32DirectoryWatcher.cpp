// Win32 backend for DirectoryWatcher: ReadDirectoryChangesW on a worker thread.

#include "../WatchBackend.hpp"

#include "aver/core/Log.hpp"

#if defined(_WIN32)

#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace aver::detail {
namespace {

// ReadDirectoryChangesW requires a DWORD-aligned buffer and refuses over 64 KB on network shares.
constexpr usize kBufferDwords = 16 * 1024;

// The change types the watch subscribes to.
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_CREATION;

// Queue cap; past it the queue is dropped and a rescan is asked for.
constexpr usize kMaxQueued = 8192;

// Converts exactly `wchars` wide characters to a UTF-8 string.
std::string narrowExact(const wchar_t* data, int wchars) {
    if (wchars <= 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, data, wchars, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<usize>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, wchars, s.data(), len, nullptr, nullptr);
    return s;
}

// Converts a UTF-8 string to a wide string.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

// Watches one directory tree with ReadDirectoryChangesW and queues raw records for the frame side.
class Win32WatchBackend final : public IWatchBackend {
public:
    // Set by the worker when it leaves its loop for a reason other than the stop event. Read from
    // the frame thread, so it is atomic rather than a plain bool.
    bool died() const override { return died_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> died_{false};

public:
    ~Win32WatchBackend() override { shutdown(); }

    // Opens the directory and starts the worker thread. Returns false if the watch cannot be made.
    bool init(const std::string& root, bool recursive) {
        recursive_ = recursive;
        root_ = root;

        dir_ = CreateFileW(widen(root).c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                           nullptr);
        if (dir_ == INVALID_HANDLE_VALUE) {
            AVER_WARN("[Watcher] declined: cannot open '{}' for watching (GetLastError {})", root,
                      (u32)GetLastError());
            dir_ = nullptr;
            return false;
        }

        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent || !stop_) {
            AVER_WARN("[Watcher] declined: could not create the watch events for '{}'", root);
            shutdown();
            return false;
        }

        armed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!armed_) {
            AVER_WARN("[Watcher] declined: could not create the arm event for '{}'", root);
            shutdown();
            return false;
        }

        buffer_.resize(kBufferDwords);
        thread_ = std::thread([this] { run(); });

        // The kernel records changes only while a read is in flight, so do not return before one is.
        if (WaitForSingleObject(armed_, 5000) != WAIT_OBJECT_0)
            AVER_WARN("[Watcher] '{}' did not arm within 5 s; early changes may have been missed", root);
        return true;
    }

    // Moves everything queued into `out`. Returns true if records were lost and a rescan is needed.
    bool drain(std::vector<RawFileEvent>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty()) {
            out.insert(out.end(), std::make_move_iterator(queue_.begin()),
                       std::make_move_iterator(queue_.end()));
            queue_.clear();
        }
        const bool over = overflow_;
        overflow_ = false;
        return over;
    }

private:
    // Stops the worker and closes every handle.
    void shutdown() {
        if (stop_) SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        if (overlapped_.hEvent) { CloseHandle(overlapped_.hEvent); overlapped_.hEvent = nullptr; }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
        if (armed_) { CloseHandle(armed_); armed_ = nullptr; }
        if (dir_) { CloseHandle(dir_); dir_ = nullptr; }
    }

    // Drops the queue and latches "rescan required", logging `why` once per overflow run.
    void flagOverflow(const char* why) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        if (!overflow_) AVER_WARN("[Watcher] lost track of '{}' ({}); a rescan is required", root_, why);
        overflow_ = true;
    }

    // Waits 50 ms. Returns false if a stop was requested while throttling.
    bool throttle() { return WaitForSingleObject(stop_, 50) == WAIT_TIMEOUT; }

    // Worker loop: issues reads, waits, and parses each completed batch until stopped.
    void run() {
        bool armedOnce = false;
        const auto arm = [&] { if (!armedOnce) { armedOnce = true; SetEvent(armed_); } };

        for (;;) {
            ResetEvent(overlapped_.hEvent);
            if (!ReadDirectoryChangesW(dir_, buffer_.data(),
                                       (DWORD)(buffer_.size() * sizeof(DWORD)),
                                       recursive_ ? TRUE : FALSE, kFilter, nullptr, &overlapped_,
                                       nullptr)) {
                const DWORD err = GetLastError();
                if (err == ERROR_NOTIFY_ENUM_DIR) {
                    flagOverflow("the OS notification buffer overflowed");
                    if (!throttle()) break;
                    continue;
                }
                if (err != ERROR_OPERATION_ABORTED)
                    AVER_WARN("[Watcher] ReadDirectoryChangesW failed ({}); watch stopped", (u32)err);
                died_.store(true, std::memory_order_release);
                arm();
                break;
            }

            arm();

            HANDLE waits[2] = {overlapped_.hEvent, stop_};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(dir_, &overlapped_);
                DWORD ignored = 0;
                GetOverlappedResult(dir_, &overlapped_, &ignored, TRUE);
                break;
            }

            DWORD bytes = 0;
            if (!GetOverlappedResult(dir_, &overlapped_, &bytes, FALSE)) {
                const DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED) break;
                if (err == ERROR_NOTIFY_ENUM_DIR) {
                    flagOverflow("the OS notification buffer overflowed");
                    if (!throttle()) break;
                    continue;
                }
                AVER_WARN("[Watcher] overlapped read failed ({}); watch stopped", (u32)err);
                break;
            }

            // A successful read of zero bytes is the documented buffer-overflow signal.
            if (bytes == 0) { flagOverflow("the OS returned an empty change buffer"); continue; }

            parse(bytes);
        }
    }

    // Turns one completed read's FILE_NOTIFY_INFORMATION chain into queued raw events.
    void parse(DWORD bytes) {
        std::vector<RawFileEvent> batch;
        const auto now = WatchClock::now();
        const u8* base = reinterpret_cast<const u8*>(buffer_.data());

        constexpr DWORD kNameOffset = (DWORD)offsetof(FILE_NOTIFY_INFORMATION, FileName);

        DWORD offset = 0;
        for (;;) {
            if (offset > bytes || bytes - offset < kNameOffset) break;
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);

            if (bytes - offset - kNameOffset < fni->FileNameLength) break;

            // FileNameLength is in BYTES and FileName is NOT null-terminated.
            const int wchars = (int)(fni->FileNameLength / sizeof(WCHAR));
            std::string path = narrowExact(fni->FileName, wchars);
            for (char& c : path) if (c == '\\') c = '/';

            bool known = true;
            RawChange kind = RawChange::Modified;
            switch (fni->Action) {
                case FILE_ACTION_ADDED:            kind = RawChange::Added; break;
                case FILE_ACTION_REMOVED:          kind = RawChange::Removed; break;
                case FILE_ACTION_MODIFIED:         kind = RawChange::Modified; break;
                case FILE_ACTION_RENAMED_OLD_NAME: kind = RawChange::RenamedFrom; break;
                case FILE_ACTION_RENAMED_NEW_NAME: kind = RawChange::RenamedTo; break;
                default:                           known = false; break;
            }
            if (known && !path.empty()) batch.push_back(RawFileEvent{kind, std::move(path), now});

            if (fni->NextEntryOffset == 0) break;
            if (fni->NextEntryOffset < kNameOffset) break;
            offset += fni->NextEntryOffset;
        }

        if (batch.empty()) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (overflow_) return;
            if (queue_.size() + batch.size() <= kMaxQueued) {
                queue_.insert(queue_.end(), std::make_move_iterator(batch.begin()),
                              std::make_move_iterator(batch.end()));
                return;
            }
        }
        flagOverflow("the frame side stopped draining and the queue filled");
    }

    HANDLE dir_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE armed_ = nullptr;
    OVERLAPPED overlapped_{};
    std::vector<DWORD> buffer_;
    std::string root_;
    bool recursive_ = true;
    std::thread thread_;

    std::mutex mutex_;
    std::vector<RawFileEvent> queue_;
    bool overflow_ = false;
};

} // namespace

// Creates a started Win32 watch backend for `root`, or nullptr if it cannot be watched.
std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool recursive) {
    auto backend = std::make_unique<Win32WatchBackend>();
    if (!backend->init(root, recursive)) return nullptr;
    return backend;
}

} // namespace aver::detail

#else

namespace aver::detail {

// No watch backend on this platform: logs once and returns nullptr.
std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool) {
    AVER_WARN("[Watcher] declined: no directory-watching backend on this platform ('{}')", root);
    return nullptr;
}

} // namespace aver::detail

#endif
