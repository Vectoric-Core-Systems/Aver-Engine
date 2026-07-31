// DirectoryWatcher, exercised against a real filesystem in a temp directory.
#include "aver/platform/DirectoryWatcher.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// This process's id, for a temp directory name two concurrent runs cannot share.
#ifdef _WIN32
#  include <process.h>
#  define AVER_TEST_PID _getpid()
#else
#  include <unistd.h>
#  define AVER_TEST_PID getpid()
#endif

using namespace aver;

static int g_failures = 0;

// Records one assertion. Counts a failure and logs it when the condition is false.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// How long every wait here runs. Well past the watcher's documented 150 ms settle window.
static constexpr int kSettleWaitMs = 700;

// Truncates a file and writes text into it.
static void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Polls for a full settle window, appending events. False if the watcher signalled overflow.
static bool drain(DirectoryWatcher& w, std::vector<FileEvent>& out, int waitMs = kSettleWaitMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (w.poll(out)) return false;               // overflow
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// How many events name this path.
static int countFor(const std::vector<FileEvent>& evs, const std::string& rel) {
    int n = 0;
    for (const FileEvent& e : evs) if (e.path == rel) ++n;
    return n;
}

// The first event naming this path, or null.
static const FileEvent* findFor(const std::vector<FileEvent>& evs, const std::string& rel) {
    for (const FileEvent& e : evs) if (e.path == rel) return &e;
    return nullptr;
}

// Runs every directory watcher check. Returns 1 if any failed.
int main() {
    AVER_INFO("=== DirectoryWatcher ===");

    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) /
        ("aver-watch-test-" + std::to_string(static_cast<long>(AVER_TEST_PID)));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) { AVER_ERROR("could not make a temp directory to watch"); return 1; }

    {
        DirectoryWatcher w;
        check(!w.start((root / "does-not-exist").string()), "start() on a missing directory returns false");
        check(!w.watching(), "and it is not watching afterwards");
        std::vector<FileEvent> evs;
        check(!w.poll(evs), "poll() on an unstarted watcher does not claim overflow");
        check(evs.empty(), "and reports nothing");
    }

    DirectoryWatcher w;
    check(w.start(root.string(), /*recursive=*/true), "start() on a real directory succeeds");
    check(w.watching(), "watching() is true after a successful start");
    check(w.root() == root.string(), "root() is what was asked for");

    {
        std::vector<FileEvent> evs;
        write(root / "one.cs", "// hello\n");
        check(drain(w, evs), "a create does not overflow the watcher");
        const FileEvent* e = findFor(evs, "one.cs");
        check(e != nullptr, "a newly written file is reported");
        if (e) check(e->kind == FileChange::Created || e->kind == FileChange::Modified,
                     "as Created (or Modified -- the header documents the ambiguity)");
        check(countFor(evs, "one.cs") == 1, "ONE event for one write, not the burst the OS emitted");
    }

    {
        std::vector<FileEvent> evs;
        write(root / "one.cs", "// hello again, with more text than before\n");
        check(drain(w, evs), "a rewrite does not overflow the watcher");
        const FileEvent* e = findFor(evs, "one.cs");
        check(e != nullptr, "a rewritten file is reported");
        if (e) check(e->kind == FileChange::Modified, "as Modified, now that the path is known");
        check(countFor(evs, "one.cs") == 1, "still ONE event, however many records the OS emitted");
    }

    {
        std::vector<FileEvent> evs;
        for (int i = 0; i < 5; ++i) {
            write(root / "burst.cs", "// write " + std::to_string(i) + "\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(drain(w, evs), "a burst does not overflow the watcher");
        check(countFor(evs, "burst.cs") == 1, "five writes 10 ms apart coalesce into ONE event");
    }

    {
        std::vector<FileEvent> evs;
        std::filesystem::rename(root / "one.cs", root / "renamed.cs", ec);
        check(!ec, "the rename itself succeeded");
        check(drain(w, evs), "a rename does not overflow the watcher");
        const FileEvent* e = findFor(evs, "renamed.cs");
        check(e != nullptr, "the rename is reported under the NEW name");
        if (e) {
            check(e->kind == FileChange::Renamed, "with kind Renamed");
            check(e->oldPath == "one.cs", "and oldPath is what it used to be called");
        }
    }

    {
        std::vector<FileEvent> evs;
        std::filesystem::remove(root / "burst.cs", ec);
        check(!ec, "the remove itself succeeded");
        check(drain(w, evs), "a delete does not overflow the watcher");
        const FileEvent* e = findFor(evs, "burst.cs");
        check(e != nullptr, "the deletion is reported");
        if (e) check(e->kind == FileChange::Deleted, "with kind Deleted");
    }

    {
        std::vector<FileEvent> evs;
        std::filesystem::create_directories(root / "Scripts", ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));   // let the mkdir settle out
        (void)drain(w, evs, 300);
        evs.clear();
        write(root / "Scripts" / "Deep.cs", "// nested\n");
        check(drain(w, evs), "a nested write does not overflow the watcher");
        const FileEvent* e = findFor(evs, "Scripts/Deep.cs");
        check(e != nullptr, "a file in a subdirectory is reported");
        if (e) check(e->path.find('\\') == std::string::npos,
                     "and its path uses '/' separators, never the platform's");
    }

    {
        std::vector<FileEvent> evs;
        check(drain(w, evs, 400), "an idle watcher does not overflow");
        check(evs.empty(), "an idle watcher reports NOTHING at all");
    }

    {
        w.stop();
        check(!w.watching(), "watching() is false after stop()");
        std::vector<FileEvent> evs;
        write(root / "after-stop.cs", "// nobody should see this\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(kSettleWaitMs));
        (void)w.poll(evs);
        check(evs.empty(), "a write after stop() produces nothing -- the worker really is gone");
    }

    std::filesystem::remove_all(root, ec);

    if (g_failures == 0) AVER_INFO("=== all directory watcher tests passed ===");
    else                 AVER_ERROR("=== {} directory watcher check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
