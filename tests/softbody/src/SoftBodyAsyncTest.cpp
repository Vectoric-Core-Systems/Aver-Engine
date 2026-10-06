// The fixed-rate worker against a synchronous replay.
//
// The worker runs the same step() on a private copy, so a queued impact must give the same bits as
// applying it and stepping by hand for the same number of steps. Unthrottled mode makes the step
// count independent of wall-clock time; a second run in real-time mode only has to finish.
#include "TestCages.hpp"

#include "aver/core/Log.hpp"
#include "aver/softbody/AsyncSolver.hpp"
#include "aver/softbody/RenderBinding.hpp"

#include <string>
#include <vector>

using namespace sbtest;

static int g_failures = 0;

// Records one assertion.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("SoftBodyAsyncTest");
    const sb::StepConfig cfg;
    sb::Impact im;
    im.point = Vec3{100, 0, 10};
    im.direction = Vec3{0, 0, -1};
    im.depthCm = 25.0f;
    im.radiusCm = 45.0f;

    AVER_INFO("a queued impact matches a synchronous replay bit for bit");
    {
        Truss seed = makeTruss(sb::Material{});
        sb::AsyncSolver worker;
        worker.enqueueImpact(im);   // queued before the thread exists: it must still run first
        sb::AsyncOptions opt;
        opt.realTime = false;
        check(worker.start(seed.cage, cfg, opt), "the worker starts");
        check(!worker.start(seed.cage, cfg, opt), "a second start while running is refused");
        check(worker.waitSettled(20000), "the worker settles");

        sb::Snapshot snap;
        check(worker.tryGetSnapshot(snap), "a snapshot is available");
        check(snap.settled && snap.stepCount > 0, "it reports settled after at least one step");
        sb::Snapshot again;
        check(!worker.tryGetSnapshot(again), "a settled worker publishes nothing more");
        worker.stop();

        Truss replay = makeTruss(sb::Material{});
        sb::applyImpact(replay.cage, im);
        u32 steps = 0;
        sb::StepResult r;
        do { r = sb::step(replay.cage, cfg); ++steps; } while (!r.settled && steps < 100000);
        check(steps == snap.stepCount, "the replay needs the same number of steps");
        check(bitwiseEqual(snap.positions, positionsOf(replay.cage)), "and ends on identical positions");

        const f32 dent = snap.positions[replay.tipTop()].z - replay.topZ0;
        check(dent < -5.0f, "the worker's cage holds a permanent dent");
    }

    AVER_INFO("commands after settling wake the worker");
    {
        Truss seed = makeTruss(sb::Material{});
        sb::AsyncSolver worker;
        sb::AsyncOptions opt;
        opt.realTime = false;
        worker.start(seed.cage, cfg, opt);
        check(worker.waitSettled(20000), "an undisturbed cage settles at once");
        sb::Snapshot snap;
        worker.tryGetSnapshot(snap);
        const u32 idleSteps = snap.stepCount;

        worker.enqueueImpact(im);
        check(worker.waitSettled(20000), "it wakes, runs the impact and settles again");
        worker.tryGetSnapshot(snap);
        check(snap.stepCount > idleSteps + 10, "it stepped through the dent");
        check(snap.brokenBeams == 0 || snap.brokenBeams < 4, "without shredding the bar");

        worker.enqueueRepair();
        check(worker.waitSettled(20000), "a repair command settles");
        worker.tryGetSnapshot(snap);
        check(std::fabs(snap.positions[seed.tipTop()].z - seed.topZ0) < 1e-4f, "and undoes the dent");
        worker.stop();
        worker.stop();
        check(!worker.running(), "stop is idempotent");
    }

    AVER_INFO("tears reach the game thread as split events");
    {
        sb::Cage sheet;
        sb::GridSpec g;
        g.cols = 5;
        g.rows = 4;
        const std::vector<u32> ids = sb::addGrid(sheet, g);
        sb::build(sheet);
        sb::RenderBinding rb = sb::RenderBinding::identity(sheet);

        sb::AsyncSolver worker;
        for (u32 x = 0; x + 1 < 5; ++x) worker.enqueueBreak(findBeam(sheet, ids[5 + x], ids[5 + x + 1]));
        sb::AsyncOptions opt;
        opt.realTime = false;
        worker.start(sheet, cfg, opt);
        check(worker.waitSettled(20000), "the worker settles after the cut");
        sb::Snapshot snap;
        check(worker.tryGetSnapshot(snap), "a snapshot arrives");
        check(snap.positions.size() == 25, "it carries the five duplicated particles");
        check(snap.splits.size() == 5, "and the five split events");
        check(snap.brokenBeams == 4, "and the broken count");

        rb.applySplits(snap.splits);
        check(rb.vertexCount() == 25, "replaying them on the render side adds five vertices");

        // The same cut applied synchronously gives the same events.
        sb::Cage local = sheet;
        for (u32 x = 0; x + 1 < 5; ++x) sb::breakBeam(local, findBeam(local, ids[5 + x], ids[5 + x + 1]));
        sb::step(local, cfg);
        bool same = local.splitLog.size() == snap.splits.size();
        for (usize i = 0; same && i < snap.splits.size(); ++i)
            same = local.splitLog[i].oldParticle == snap.splits[i].oldParticle &&
                   local.splitLog[i].newParticle == snap.splits[i].newParticle &&
                   local.splitLog[i].triangles == snap.splits[i].triangles;
        check(same, "identical to the synchronous tear");
    }

    AVER_INFO("the real-time worker settles too");
    {
        Truss seed = makeTruss(sb::Material{});
        sb::AsyncSolver worker;
        worker.enqueueImpact(im);
        sb::AsyncOptions opt;
        opt.hz = 240.0f;
        worker.start(seed.cage, cfg, opt);
        check(worker.waitSettled(20000), "paced at 240 Hz it still settles");
        sb::Snapshot snap;
        check(worker.tryGetSnapshot(snap) && snap.settled, "and says so");
        worker.stop();
    }

    AVER_INFO(g_failures ? "SoftBodyAsyncTest: {} FAILURES" : "SoftBodyAsyncTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
