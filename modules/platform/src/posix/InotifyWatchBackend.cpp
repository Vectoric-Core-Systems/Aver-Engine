// The Linux IWatchBackend: inotify. The Win32 one is src/win32/Win32DirectoryWatcher.cpp, and the
// contract both meet is WatchBackend.hpp's; DirectoryWatcher.cpp does the debouncing, coalescing and
// rename pairing above either.
//
// WHAT inotify DOES NOT DO THAT ReadDirectoryChangesW DOES, and what this file does about it:
//
//   * IT IS NOT RECURSIVE. One watch per directory, so the tree is walked at start and every
//     directory created (or moved in) afterwards gets a watch of its own. A file written into a new
//     directory before its watch exists would be missed, so a new directory's contents are reported
//     as Added when the watch is placed -- what the Win32 recursive watch would have seen anyway.
//   * A RENAME IS TWO EVENTS joined by a cookie (IN_MOVED_FROM, IN_MOVED_TO). They are emitted as the
//     RenamedFrom/RenamedTo pair WatchBackend.hpp asks for when both halves are inside the tree; a
//     move out of the tree is a Removed, a move in is an Added -- again what Windows reports.
//   * A WATCHED DIRECTORY KNOWS ITS NAME ONLY HERE. Renaming one moves every watch below it, so the
//     wd -> path table is rewritten by prefix.
//   * THE KERNEL QUEUE CAN OVERFLOW (IN_Q_OVERFLOW) and the per-user watch limit can run out
//     (fs.inotify.max_user_watches). The first is a rescan, exactly like ERROR_NOTIFY_ENUM_DIR; the
//     second is logged once, and the directories past the limit are not watched.
#include "../WatchBackend.hpp"
#include "aver/core/Log.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace aver::detail {
namespace {

constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                           IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;
// Win32DirectoryWatcher.cpp's bound on undrained events before a rescan is asked for instead.
constexpr usize kMaxQueued = 8192;

class InotifyWatchBackend final : public IWatchBackend {
public:
    ~InotifyWatchBackend() override { shutdown(); }

    bool died() const override { return died_.load(std::memory_order_acquire); }

    bool init(const std::string& root, bool recursive) {
        root_ = std::filesystem::path(root).lexically_normal().string();
        while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
        recursive_ = recursive;
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        stop_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd_ < 0 || stop_ < 0) {
            AVER_WARN("[Watcher] declined: inotify unavailable for '{}' ({})", root, std::strerror(errno));
            shutdown();
            return false;
        }
        if (!addWatch("", false)) {
            AVER_WARN("[Watcher] declined: cannot watch '{}' ({})", root, std::strerror(errno));
            shutdown();
            return false;
        }
        // Watches are placed synchronously, so the tree is armed when this returns -- Win32's arm
        // event has nothing to wait for here.
        thread_ = std::thread([this] { run(); });
        return true;
    }

    bool drain(std::vector<RawFileEvent>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty()) {
            out.insert(out.end(), std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
            queue_.clear();
        }
        const bool over = overflow_;
        overflow_ = false;
        return over;
    }

private:
    // Watches `rel` (relative to the root, "" = the root) and, when recursive, every directory below
    // it. With `reportContents`, everything found below is queued as Added (see the file header).
    bool addWatch(const std::string& rel, bool reportContents) {
        const std::string abs = rel.empty() ? root_ : root_ + "/" + rel;
        const int wd = inotify_add_watch(fd_, abs.c_str(), kMask);
        if (wd < 0) {
            if (errno == ENOSPC && !limitLogged_) {
                limitLogged_ = true;
                AVER_WARN("[Watcher] out of inotify watches under '{}' (raise fs.inotify.max_user_watches); "
                          "directories past the limit are not watched", root_);
            }
            return false;
        }
        paths_[wd] = rel;
        if (!recursive_) return true;
        // increment(ec), not a range-for: this runs on the worker thread, and a directory deleted mid-listing
        // (a build's temp folder) would throw from operator++ with nothing to catch it.
        std::error_code ec;
        std::filesystem::directory_iterator it(abs, std::filesystem::directory_options::skip_permission_denied, ec);
        for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const std::string child = (rel.empty() ? "" : rel + "/") + it->path().filename().string();
            std::error_code tec;
            const bool isDir = it->is_directory(tec) && !it->is_symlink(tec);
            if (reportContents) batch_.push_back(RawFileEvent{RawChange::Added, child, WatchClock::now()});
            if (isDir) addWatch(child, reportContents);
        }
        return true;
    }

    // A watched directory was renamed from `from` to `to`: every watch at or below it follows.
    void renameWatches(const std::string& from, const std::string& to) {
        for (auto& [wd, p] : paths_) {
            if (p == from) p = to;
            else if (p.size() > from.size() && p.compare(0, from.size(), from) == 0 && p[from.size()] == '/')
                p = to + p.substr(from.size());
        }
    }

    void forgetWatchesUnder(const std::string& rel) {
        for (auto it = paths_.begin(); it != paths_.end();) {
            const std::string& p = it->second;
            const bool under = p == rel || (p.size() > rel.size() && p.compare(0, rel.size(), rel) == 0 && p[rel.size()] == '/');
            if (under) { inotify_rm_watch(fd_, it->first); it = paths_.erase(it); }
            else ++it;
        }
    }

    void emit(RawChange kind, std::string path) {
        if (!path.empty()) batch_.push_back(RawFileEvent{kind, std::move(path), WatchClock::now()});
    }

    // A MOVED_FROM whose MOVED_TO has not been seen: inside this read it is still a rename in
    // progress, and if the read ends without its partner the item left the tree.
    struct HeldMove { uint32_t cookie = 0; std::string path; bool isDir = false; bool held = false; };

    void settleHeldMove() {
        if (!held_.held) return;
        if (held_.isDir) forgetWatchesUnder(held_.path);
        emit(RawChange::Removed, held_.path);
        held_ = HeldMove{};
    }

    void handle(const inotify_event& ev) {
        if (ev.mask & IN_Q_OVERFLOW) { flagOverflow("the inotify queue overflowed"); return; }
        const auto it = paths_.find(ev.wd);
        if (ev.mask & IN_IGNORED) { if (it != paths_.end()) paths_.erase(it); return; }
        if (it == paths_.end()) return;
        const std::string dir = it->second;

        if ((ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) && dir.empty()) {
            noteExit("the watched root itself was deleted or moved");
            return;
        }
        if (ev.len == 0 || ev.name[0] == '\0') return;   // an event about the directory itself
        const std::string path = (dir.empty() ? "" : dir + "/") + ev.name;
        const bool isDir = (ev.mask & IN_ISDIR) != 0;

        // Anything but the partner of a held MOVED_FROM settles it as a move out of the tree.
        if (held_.held && !((ev.mask & IN_MOVED_TO) && ev.cookie == held_.cookie)) settleHeldMove();

        if (ev.mask & IN_MOVED_FROM) {
            held_ = HeldMove{ev.cookie, path, isDir, true};
        } else if (ev.mask & IN_MOVED_TO) {
            if (held_.held && ev.cookie == held_.cookie) {
                emit(RawChange::RenamedFrom, held_.path);
                emit(RawChange::RenamedTo, path);
                if (isDir) renameWatches(held_.path, path);
                held_ = HeldMove{};
            } else {
                emit(RawChange::Added, path);
                if (isDir && recursive_) addWatch(path, true);
            }
        } else if (ev.mask & IN_CREATE) {
            emit(RawChange::Added, path);
            if (isDir && recursive_) addWatch(path, true);
        } else if (ev.mask & IN_DELETE) {
            emit(RawChange::Removed, path);
        } else if (ev.mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
            if (!isDir) emit(RawChange::Modified, path);
        }
    }

    void run() {
        std::vector<char> buf(64 * 1024);
        for (;;) {
            pollfd fds[2] = {{fd_, POLLIN, 0}, {stop_, POLLIN, 0}};
            // While a MOVED_FROM is held, wait only briefly for its partner: the kernel queues the two
            // back to back, but a read can end between them.
            const int timeout = held_.held ? 20 : -1;
            const int n = ::poll(fds, 2, timeout);
            if (n < 0) {
                if (errno == EINTR) continue;
                noteExit("poll on the inotify descriptor failed");
                break;
            }
            if (fds[1].revents & POLLIN) break;   // asked to stop; not a death
            if (n == 0) { settleHeldMove(); publish(); continue; }

            const ssize_t got = ::read(fd_, buf.data(), buf.size());
            if (got < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                noteExit("reading the inotify descriptor failed");
                break;
            }
            for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(inotify_event)) <= got;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(buf.data() + off);
                handle(*ev);
                off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            }
            publish();
            if (died()) break;
            if (const int kill = killAfterBatches(); kill > 0 && ++batches_ >= kill) {
                AVER_WARN("[Watcher] AVER_WATCHER_KILL_AFTER={}: leaving the worker loop as a failed read would", kill);
                noteExit("a simulated read failure");
                break;
            }
        }
    }

    void publish() {
        if (batch_.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (overflow_) { batch_.clear(); return; }
            if (queue_.size() + batch_.size() <= kMaxQueued) {
                queue_.insert(queue_.end(), std::make_move_iterator(batch_.begin()), std::make_move_iterator(batch_.end()));
                batch_.clear();
                return;
            }
        }
        batch_.clear();
        flagOverflow("the frame side stopped draining and the queue filled");
    }

    void flagOverflow(const char* why) {
        batch_.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        if (!overflow_) AVER_WARN("[Watcher] lost track of '{}' ({}); a rescan is required", root_, why);
        overflow_ = true;
    }

    void noteExit(const char* why) {
        died_.store(true, std::memory_order_release);
        AVER_WARN("[Watcher] '{}' stopped watching: {}. Hot reload is over for this session unless the "
                  "caller re-arms.", root_, why);
    }

    // The test hook Win32DirectoryWatcher.cpp has: leave the worker after N batches, as a failure would.
    static int killAfterBatches() {
        const char* v = std::getenv("AVER_WATCHER_KILL_AFTER");
        return v && *v ? std::atoi(v) : 0;
    }

    void shutdown() {
        if (stop_ >= 0) { const uint64_t one = 1; (void)!::write(stop_, &one, sizeof one); }
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (stop_ >= 0) { ::close(stop_); stop_ = -1; }
    }

    std::string root_;
    bool recursive_ = true;
    int fd_ = -1;
    int stop_ = -1;
    bool limitLogged_ = false;
    int batches_ = 0;
    std::unordered_map<int, std::string> paths_;   // worker thread only (and init, before it starts)
    std::vector<RawFileEvent> batch_;              // worker thread only
    HeldMove held_;                                // worker thread only
    std::thread thread_;
    std::mutex mutex_;
    std::vector<RawFileEvent> queue_;
    bool overflow_ = false;
    std::atomic<bool> died_{false};
};

} // namespace

std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool recursive) {
    auto backend = std::make_unique<InotifyWatchBackend>();
    if (!backend->init(root, recursive)) return nullptr;
    return backend;
}

} // namespace aver::detail
