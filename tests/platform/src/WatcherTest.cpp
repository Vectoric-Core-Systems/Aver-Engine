// DirectoryWatcher, against a real filesystem.
//
// This component shipped complete -- debouncing, coalescing, rename pairing, overflow signalling --
// and with NO consumer and NO test. The editor now depends on it to notice a save made in Visual
// Studio, so the properties below stopped being documentation and became load-bearing.
//
// It is a real-filesystem test rather than a mocked one on purpose. Every hard case here is the
// operating system's behaviour, not the class's: Windows reports a safe save (write temp, replace
// target, rename into place) as a rename with no removal record, it reports one logical write as
// several records, and it collapses a rename pair into two records that only mean something
// together. A fake backend would be a test of the fake.
//
// TIMING is the one thing a test like this can get wrong in a way that wastes a day. The rule
// followed throughout: never assert that an event has NOT arrived yet -- that races the settle
// timer and fails on a loaded machine. Assert only what has arrived after waiting longer than the
// settle window, and use a generous window rather than a tight one.
#include "aver/platform/DirectoryWatcher.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// For the process id only, which is what keeps two concurrent runs out of each other's temp
// directory. Nothing else here is platform-specific -- the watcher's whole point is that its
// interface is not.
#ifdef _WIN32
#  include <process.h>
#  define AVER_TEST_PID _getpid()
#else
#  include <unistd.h>
#  define AVER_TEST_PID getpid()
#endif

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// The settle window the watcher documents is 150 ms. Everything here waits WELL past it: a test that
// waits 160 ms passes on an idle machine and fails under load, and a flaky test is worse than none
// because it teaches people to re-run rather than to read.
static constexpr int kSettleWaitMs = 700;

static void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Drain until the watcher has been quiet for a full settle window. Returns false if the watcher
// signalled overflow, which no case here should provoke.
static bool drain(DirectoryWatcher& w, std::vector<FileEvent>& out, int waitMs = kSettleWaitMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (w.poll(out)) return false;               // overflow
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// How many events name this path? One save must produce exactly one, which is the entire point of
// the debounce and the one property a consumer's correctness rests on.
static int countFor(const std::vector<FileEvent>& evs, const std::string& rel) {
    int n = 0;
    for (const FileEvent& e : evs) if (e.path == rel) ++n;
    return n;
}

static const FileEvent* findFor(const std::vector<FileEvent>& evs, const std::string& rel) {
    for (const FileEvent& e : evs) if (e.path == rel) return &e;
    return nullptr;
}

int main() {
    AVER_INFO("=== DirectoryWatcher ===");

    // A directory of our own under the system temp, removed at the end. Named with the process id so
    // two runs on one machine cannot collide.
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) /
        ("aver-watch-test-" + std::to_string(static_cast<long>(AVER_TEST_PID)));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) { AVER_ERROR("could not make a temp directory to watch"); return 1; }

    // ---- it DECLINES cleanly ------------------------------------------------------------------
    // The contract says a missing directory logs once and returns false, so the editor carries on
    // with no watch exactly as it does with no project. A watcher that threw or aborted here would
    // take the editor down on any project whose content folder had been moved.
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

    // ---- a new file arrives -------------------------------------------------------------------
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

    // ---- a rewrite is Modified, and still exactly one event ------------------------------------
    //
    // The heart of it. An editor writing a file produces several OS records; a consumer that reloaded
    // per record would re-read a half-written file, and this is the assertion that says it will not.
    {
        std::vector<FileEvent> evs;
        write(root / "one.cs", "// hello again, with more text than before\n");
        check(drain(w, evs), "a rewrite does not overflow the watcher");
        const FileEvent* e = findFor(evs, "one.cs");
        check(e != nullptr, "a rewritten file is reported");
        if (e) check(e->kind == FileChange::Modified, "as Modified, now that the path is known");
        check(countFor(evs, "one.cs") == 1, "still ONE event, however many records the OS emitted");
    }

    // ---- a BURST of writes coalesces to one ----------------------------------------------------
    //
    // Five writes in quick succession is not a contrived case: it is what a formatter, a code
    // generator or a save-all does. Five reloads of one file is five parses and five preview
    // rebuilds for one user action.
    {
        std::vector<FileEvent> evs;
        for (int i = 0; i < 5; ++i) {
            write(root / "burst.cs", "// write " + std::to_string(i) + "\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(drain(w, evs), "a burst does not overflow the watcher");
        check(countFor(evs, "burst.cs") == 1, "five writes 10 ms apart coalesce into ONE event");
    }

    // ---- a rename carries the OLD name ---------------------------------------------------------
    //
    // Without the pairing this is two unrelated records and a consumer sees a delete and an
    // unrelated create -- which for an open editor tab means closing it and opening a different one.
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

    // ---- a delete is a delete ------------------------------------------------------------------
    {
        std::vector<FileEvent> evs;
        std::filesystem::remove(root / "burst.cs", ec);
        check(!ec, "the remove itself succeeded");
        check(drain(w, evs), "a delete does not overflow the watcher");
        const FileEvent* e = findFor(evs, "burst.cs");
        check(e != nullptr, "the deletion is reported");
        if (e) check(e->kind == FileChange::Deleted, "with kind Deleted");
    }

    // ---- SUBDIRECTORIES, because a project's scripts live in one -------------------------------
    //
    // recursive=true is what the editor passes, and Content/Scripts/Foo.cs is the only path anybody
    // actually cares about. A watcher that only saw the root would be watching the wrong thing.
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
        // The separator matters: the path is used as a map key and joined back onto the root, and
        // the header promises forward slashes regardless of what the OS handed over.
        if (e) check(e->path.find('\\') == std::string::npos,
                     "and its path uses '/' separators, never the platform's");
    }

    // ---- a quiet watcher reports nothing -------------------------------------------------------
    //
    // The frame loop calls poll() sixty times a second forever. If an idle watcher invented events,
    // the editor would reload every open file continuously and nobody would be able to type.
    {
        std::vector<FileEvent> evs;
        check(drain(w, evs, 400), "an idle watcher does not overflow");
        check(evs.empty(), "an idle watcher reports NOTHING at all");
    }

    // ---- stop() is final -----------------------------------------------------------------------
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
