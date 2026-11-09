// HlodBuildJobTest -- the pure side of World Settings > Level Streaming > Build HLOD (sandbox/src/HlodBuildJob.hpp): when a bake is refused and
// why, the summary line, and the background job's start / progress / cancel / result handover with a stand-in bake.
#include "HlodBuildJob.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace aver;
namespace ed = aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool waitFor(ed::HlodBuildJob& job, ed::HlodJobResult& out) {
    for (int i = 0; i < 2000; ++i) {
        if (job.poll(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

static void testPrerequisites() {
    AVER_INFO("=== prerequisites ===");
    fmt::OcStreamSettings s;
    check(!ed::hlodBlockedReason(s, false).empty(), "no level open is refused");
    check(ed::hlodBlockedReason(s, true).find("Stream this level") != std::string::npos, "a level that does not stream says to turn that on");
    s.enabled = true;
    s.cellCm = 0.0f;
    check(!ed::hlodBlockedReason(s, true).empty(), "a missing cell size is refused");
    s.cellCm = 6400.0f;
    check(ed::hlodBlockedReason(s, true).empty(), "a streaming level with a cell size can be baked");
    s.loadCm = 20000.0f;
    check(ed::defaultHlodCm(s) == 60000.0f, "the default HLOD distance is three load distances");
    s.hlodCm = 90000.0f;
    check(ed::defaultHlodCm(s) == 90000.0f, "an authored distance wins");
}

static void testSummary() {
    AVER_INFO("=== summary ===");
    fmt::OcHlodReport r;
    r.cells = 12; r.built = 9; r.fresh = 3; r.trianglesIn = 4210; r.trianglesOut = 980;
    check(ed::hlodSummary(r) == "12 cells: 9 built, 3 up to date; 4210 -> 980 triangles", "the plain line");
    r.cells = 1; r.removed = 2; r.failed = 1; r.excludedByLayer = 5; r.cancelled = true;
    const std::string t = ed::hlodSummary(r);
    check(t.find("1 cell:") == 0 && t.find("2 removed") != std::string::npos && t.find("1 failed") != std::string::npos &&
          t.find("5 left out by data layers") != std::string::npos && t.find("cancelled") != std::string::npos, "removed, failed, layers and cancel are named");
}

static void testJob() {
    AVER_INFO("=== job ===");
    ed::HlodBuildJob job;
    ed::HlodJobResult res;
    check(!job.poll(res) && !job.running(), "a fresh job has nothing to poll");

    check(job.start([](const std::atomic<bool>&, std::atomic<u32>& done, std::atomic<u32>& total) {
        total = 4;
        for (u32 i = 0; i < 4; ++i) done = i + 1;
        ed::HlodJobResult r;
        r.ok = true; r.hlodCm = 123.0f; r.report.built = 4;
        return r;
    }), "a job starts");
    check(waitFor(job, res) && res.ok && res.hlodCm == 123.0f && res.report.built == 4, "the result arrives on poll");
    check(job.done() == 4 && job.total() == 4 && job.fraction() == 1.0f, "progress counted to the end");
    check(!job.poll(res), "a result is handed over once");

    // cancel: the stand-in loops until told to stop
    check(job.start([](const std::atomic<bool>& cancel, std::atomic<u32>& done, std::atomic<u32>& total) {
        total = 1000000;
        while (!cancel.load()) { ++done; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        ed::HlodJobResult r;
        r.ok = true; r.report.cancelled = true;
        return r;
    }), "a second job starts after the first was collected");
    check(!job.start([](const std::atomic<bool>&, std::atomic<u32>&, std::atomic<u32>&) { return ed::HlodJobResult{}; }), "a second start while one runs is refused");
    job.cancel();
    check(waitFor(job, res) && res.report.cancelled, "cancel stops the bake and the partial result is returned");

    // a throwing bake becomes a failed result
    job.start([](const std::atomic<bool>&, std::atomic<u32>&, std::atomic<u32>&) -> ed::HlodJobResult { throw std::runtime_error("disk full"); });
    check(waitFor(job, res) && !res.ok && res.why == "disk full", "an exception is reported, not thrown across the thread");

    // destroying a running job waits for it
    {
        ed::HlodBuildJob scoped;
        scoped.start([](const std::atomic<bool>& cancel, std::atomic<u32>&, std::atomic<u32>&) {
            while (!cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return ed::HlodJobResult{};
        });
    }
    check(true, "destroying a running job cancels and joins it");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("HLOD build job");
    testPrerequisites();
    testSummary();
    testJob();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
