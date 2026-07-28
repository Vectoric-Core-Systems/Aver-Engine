// The Win32 backend for DirectoryWatcher: ReadDirectoryChangesW on a worker thread.
//
// Everything OS-specific about watching a directory is in this file, and nothing else in the
// engine includes it. The coalescing policy lives in DirectoryWatcher.cpp, which knows nothing
// about Windows.

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

// 64 KB. ReadDirectoryChangesW requires a DWORD-aligned buffer, which is why it is a vector of
// DWORD rather than of u8, and the API refuses buffers over 64 KB on network shares - so this is
// the largest size that works everywhere rather than the largest size that works here.
constexpr usize kBufferDwords = 16 * 1024;

// Watching name changes AND last-write AND size deliberately over-reports: a single write can
// surface as two records. That is the debouncer's problem, and the alternative - watching only
// last-write - misses a file whose size changes without its timestamp resolution moving.
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_CREATION;

// If the frame side stops polling, the queue must not grow without bound. Passing this is treated
// exactly like an OS buffer overflow: the detail is worthless, so say "rescan" and drop it.
constexpr usize kMaxQueued = 8192;

std::string narrowExact(const wchar_t* data, int wchars) {
    if (wchars <= 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, data, wchars, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<usize>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, wchars, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

class Win32WatchBackend final : public IWatchBackend {
public:
    ~Win32WatchBackend() override { shutdown(); }

    bool init(const std::string& root, bool recursive) {
        recursive_ = recursive;
        root_ = root;

        // FILE_FLAG_BACKUP_SEMANTICS is what makes CreateFileW open a DIRECTORY at all, and
        // FILE_SHARE_DELETE matters because without it the watch would stop anyone renaming or
        // deleting the folder we are watching.
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

        // Manual-reset, because it is waited on together with the stop event and is reset
        // explicitly before each read is issued.
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent || !stop_) {
            AVER_WARN("[Watcher] declined: could not create the watch events for '{}'", root);
            shutdown();
            return false;
        }

        // ARMED, manual-reset. See the wait below -- this is the whole of the fix for the startup
        // race and it has to exist before the thread that sets it.
        armed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!armed_) {
            AVER_WARN("[Watcher] declined: could not create the arm event for '{}'", root);
            shutdown();
            return false;
        }

        buffer_.resize(kBufferDwords);
        thread_ = std::thread([this] { run(); });

        // DO NOT RETURN UNTIL THE FIRST READ IS OUTSTANDING.
        //
        // The kernel records changes to a directory only while a ReadDirectoryChangesW is in flight
        // on its handle; nothing is retained from before the first one is issued. That call happens
        // at the top of run(), on the worker thread -- so a start() that returned as soon as the
        // thread was spawned handed back a watcher with a live-looking `watching()` and a window,
        // however short, in which every change was silently dropped.
        //
        // Found by WatcherTest: the first file written after start() was never reported and the
        // second always was, which is this race exactly and nothing else.
        //
        // The timeout is a backstop against a thread that cannot start at all, not a tuning knob --
        // the wait is normally microseconds. Timing out is reported and does NOT fail the start: a
        // watch that armed late is worth more to the editor than no watch, and poll() is honest
        // either way.
        if (WaitForSingleObject(armed_, 5000) != WAIT_OBJECT_0)
            AVER_WARN("[Watcher] '{}' did not arm within 5 s; early changes may have been missed", root);
        return true;
    }

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
    void shutdown() {
        if (stop_) SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        // Only after the join, because the kernel may still be writing into buffer_/overlapped_
        // for as long as a read is in flight.
        if (overlapped_.hEvent) { CloseHandle(overlapped_.hEvent); overlapped_.hEvent = nullptr; }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
        if (armed_) { CloseHandle(armed_); armed_ = nullptr; }
        if (dir_) { CloseHandle(dir_); dir_ = nullptr; }
    }

    // `why` names which of the two ways of losing records happened, because they mean different
    // things to whoever reads the log: the OS one says the machine changed faster than the watch
    // could report, the queue one says the FRAME SIDE stopped draining.
    void flagOverflow(const char* why) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        // Only on the transition, so a storm that keeps overflowing logs once and not per read.
        if (!overflow_) AVER_WARN("[Watcher] lost track of '{}' ({}); a rescan is required", root_, why);
        overflow_ = true;
    }

    // Returns false if a stop was requested while throttling.
    bool throttle() { return WaitForSingleObject(stop_, 50) == WAIT_TIMEOUT; }

    void run() {
        // Releases init()'s wait exactly once, whatever happens next. Deliberately fired on EVERY
        // exit from the loop as well as on the first successful read: a first read that fails would
        // otherwise leave start() blocked for the full timeout on a watch that was already dead.
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
                    // "Too much changed to tell you what" - the caller must rescan. Throttle so a
                    // storm cannot spin this thread, and stay responsive to stop while doing it.
                    flagOverflow("the OS notification buffer overflowed");
                    if (!throttle()) break;
                    continue;
                }
                if (err != ERROR_OPERATION_ABORTED)
                    AVER_WARN("[Watcher] ReadDirectoryChangesW failed ({}); watch stopped", (u32)err);
                arm();
                break;
            }

            // The read is now OUTSTANDING, which is the instant the kernel begins recording changes
            // for this handle. That -- not the thread starting, and not the handle opening -- is what
            // start() has to wait for.
            arm();

            HANDLE waits[2] = {overlapped_.hEvent, stop_};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                // Stop, or the wait itself failed. Cancel the outstanding read and wait for the
                // kernel to actually finish with the buffer before returning.
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

            // A successful read of ZERO bytes is the documented buffer-overflow signal: the
            // records did not fit, so Windows discarded ALL of them. It is not a spurious wakeup.
            if (bytes == 0) { flagOverflow("the OS returned an empty change buffer"); continue; }

            parse(bytes);
        }
    }

    void parse(DWORD bytes) {
        std::vector<RawFileEvent> batch;
        const auto now = WatchClock::now();
        const u8* base = reinterpret_cast<const u8*>(buffer_.data());

        constexpr DWORD kNameOffset = (DWORD)offsetof(FILE_NOTIFY_INFORMATION, FileName);

        DWORD offset = 0;
        for (;;) {
            if (offset > bytes || bytes - offset < kNameOffset) break;
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);

            // Bound the NAME as well as the header. The kernel fills this buffer, so a length that
            // runs past the read is not an expected condition — but the alternative to checking is
            // an overread whose only symptom is a path with rubbish on the end, and the check is a
            // subtraction. Both comparisons avoid `offset + n` so a hostile length cannot wrap.
            if (bytes - offset - kNameOffset < fni->FileNameLength) break;

            // FileNameLength is in BYTES and FileName is NOT null-terminated. Treating it as a
            // wide C string yields paths with whatever happened to follow them in the buffer.
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

            // A non-advancing NextEntryOffset would spin this loop forever on a malformed buffer.
            if (fni->NextEntryOffset == 0) break;
            if (fni->NextEntryOffset < kNameOffset) break;
            offset += fni->NextEntryOffset;
        }

        if (batch.empty()) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (overflow_) return;  // already asked for a rescan; the detail is worthless
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
    // Set by the worker once its first read is outstanding; waited on by init(). See there.
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

std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool recursive) {
    auto backend = std::make_unique<Win32WatchBackend>();
    if (!backend->init(root, recursive)) return nullptr;
    return backend;
}

} // namespace aver::detail

#else

namespace aver::detail {

std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool) {
    AVER_WARN("[Watcher] declined: no directory-watching backend on this platform ('{}')", root);
    return nullptr;
}

} // namespace aver::detail

#endif
