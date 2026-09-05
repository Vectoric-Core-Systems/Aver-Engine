// Actor-driven residency: predictive loading, and the boundary hold when the loader cannot keep up.
// Exit code = failure count.
//
// TWO PROPERTIES, and they are opposites on purpose:
//
//   GIVEN ENOUGH BUDGET, a mover at speed never arrives somewhere unloaded -- the residency leads
//   along its velocity by exactly the time the budget needs to fill the ring in front of it.
//
//   GIVEN TOO LITTLE, it is HELD at the last loaded chunk rather than let through. The alternatives
//   are a synchronous load (an unbounded frame hitch) or a character standing on a chunk that does
//   not exist (a fall through the world). Holding is the only outcome that is both recoverable and
//   visible in a test.
//
// A test that only checked the first would pass on a streamer that quietly lets a starved actor walk
// into nothing, which is the failure that actually reaches a player.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/ChunkStreamer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// A generator, so the world is unbounded and a flight can run as far as it likes without a fixture
// to cook first. Every chunk on the surface layer exists.
static GeneratorSettings genSettings() {
    GeneratorSettings g;
    g.worldSeed = 0xAC70'0001ull;
    g.samplesPerAxis = 2;      // small payloads: this test is about residency, not content
    g.threshold = 0.0f;        // every candidate qualifies, so every surface chunk has something
    g.featureSizeCm = 1600.0f;
    return g;
}

int main() {
    const i32 S = kDefaultChunkSizeCm;
    scene::World& w = scene::World::instance();
    GeneratedChunkSource src(genSettings());

    // ---- a fast mover with enough budget never outruns the loader --------------------------------
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 3;
        s.evictRadius = 5;
        s.verticalRadius = 0;
        s.loadBudget = 8;
        s.evictBudget = 8;
        s.leadSeconds = 1.5f;
        st.setSettings(s);
        st.setSource(&src);

        // 60 Hz and genuinely fast: 60 000 cm/s is 600 m/s, and at 16 m chunks that crosses a chunk
        // every 1.6 frames. Chosen because the CONTROL below must actually fail at this speed -- at
        // 12 000 cm/s a budget of 1 per frame keeps up unaided, so the lead would have looked
        // effective while doing nothing.
        const f32 dt = 1.0f / 60.0f;
        const f32 speed = 60000.0f;
        Vec3 pos{0, 0, 0};
        const Vec3 vel{speed, 0, 0};

        // WARM UP FIRST, and this is what a game does at level load rather than a concession to the
        // test. Nothing is resident on frame zero, so a budget of 8 cannot fill a neighbourhood
        // before a 600 m/s mover has already left it: the first run measured exactly 2 arrivals in
        // unloaded space, both in the opening frames, and none afterwards. Spawning a player and
        // moving at full speed in the same frame is not a case the streamer can serve, and pretending
        // it is would mean loosening the steady-state assertion to hide a transient.
        u32 warmUpdates = 0;
        st.setSources({StreamSource{pos, vel}});
        for (int i = 0; i < 200; ++i) {
            const StreamStats ws = st.update(w, bodies);
            w.flush();
            ++warmUpdates;
            if (ws.pendingLoads == 0) break;
        }
        check(st.isResidentAt(pos), "after warm-up the mover's own chunk is loaded");
        AVER_INFO("   note  warm-up took {} updates before the wanted set was satisfied", warmUpdates);

        u32 steps = 0, arrivedUnloaded = 0;
        for (int i = 0; i < 600; ++i) {   // ten seconds, 6 km
            st.setSources({StreamSource{pos, vel}});
            std::vector<i32> freed;
            st.update(w, bodies, &freed);
            w.flush();

            const Vec3 next{pos.x + vel.x * dt, pos.y, pos.z};
            // The question the DONE-WHEN asks: is where it is about to BE already loaded?
            if (!st.isResidentAt(next)) ++arrivedUnloaded;
            pos = next;
            ++steps;
        }
        check(steps == 600, "the flight ran to completion");
        check(arrivedUnloaded == 0,
              "AT 600 m/s THE MOVER NEVER ARRIVES IN AN UNLOADED CHUNK -- the lead covers the budget");
        AVER_INFO("   note  {} steps at {:.0f} cm/s, {} arrivals in unloaded space, {} chunks resident",
                  steps, speed, arrivedUnloaded, st.residentCount());

        std::vector<i32> freed;
        st.unloadAll(w, bodies, freed);
        w.flush();
    }

    // ---- without the lead, the same flight DOES outrun it -----------------------------------------
    //
    // The control. Without this, "never arrives unloaded" might just mean the radius was generous,
    // and the lead would be untested decoration.
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 3;
        s.evictRadius = 5;
        s.verticalRadius = 0;
        s.loadBudget = 1;        // starved, so the ring in front fills slowly
        s.evictBudget = 8;
        s.leadSeconds = 0.0f;    // and no prediction at all
        st.setSettings(s);
        st.setSource(&src);

        const f32 dt = 1.0f / 60.0f;
        const Vec3 vel{60000.0f, 0, 0};
        Vec3 pos{0, 0, 0};
        u32 arrivedUnloaded = 0;
        for (int i = 0; i < 600; ++i) {
            st.setSources({StreamSource{pos, Vec3{0, 0, 0}}});   // no velocity reported either
            std::vector<i32> freed;
            st.update(w, bodies, &freed);
            w.flush();
            const Vec3 next{pos.x + vel.x * dt, pos.y, pos.z};
            if (!st.isResidentAt(next)) ++arrivedUnloaded;
            pos = next;
        }
        check(arrivedUnloaded > 0,
              "starved and unled, the SAME flight does reach unloaded space -- the lead is doing real work");
        AVER_INFO("   note  control: {} arrivals in unloaded space without lead or budget", arrivedUnloaded);

        std::vector<i32> freed;
        st.unloadAll(w, bodies, freed);
        w.flush();
    }

    // ---- the boundary hold ---------------------------------------------------------------------------
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 1;        // a tiny loaded island
        s.evictRadius = 2;
        s.verticalRadius = 0;
        s.loadBudget = 0;        // fill it completely, then never load again
        s.evictBudget = 0;
        s.leadSeconds = 0.0f;
        st.setSettings(s);
        st.setSource(&src);

        const Vec3 home{800.0f, 800.0f, 0.0f};
        st.setSources({StreamSource{home}});
        std::vector<i32> freed;
        st.update(w, bodies, &freed);
        w.flush();
        check(st.residentCount() > 0, "a small island of chunks is resident");
        check(st.isResidentAt(home), "including the one the mover stands in");

        // Now take the source away entirely, so nothing further will ever load, and try to walk out.
        // Far beyond the island: the destination is certainly not resident.
        const Vec3 target{home.x + 20.0f * S, home.y, home.z};
        check(!st.isResidentAt(target), "the destination is not resident");

        const Vec3 held = st.clampToResident(home, target);
        check(st.isResidentAt(held), "THE MOVER IS HELD SOMEWHERE LOADED rather than let through");
        check(held.x > home.x, "...but it did move, as far as the loaded space allowed");
        check(held.x < target.x, "...and not all the way");

        // The hold must be STABLE: clamping again from where it was held must not creep further.
        const Vec3 again = st.clampToResident(held, target);
        check(st.isResidentAt(again), "clamping again still lands somewhere loaded");
        check(std::fabs(again.x - held.x) < 2.0f,
              "...and does not creep -- the margin stops it re-triggering every frame");

        // A destination that IS resident passes through untouched, or the clamp would be a tax on
        // every ordinary step rather than a rare correction.
        const Vec3 near{home.x + 10.0f, home.y, home.z};
        const Vec3 free = st.clampToResident(home, near);
        check(free.x == near.x && free.y == near.y && free.z == near.z,
              "a move that stays inside loaded space is returned UNCHANGED");

        // And a mover already outside gets no help, which is the honest answer rather than
        // teleporting it somewhere.
        const Vec3 stranded{home.x + 40.0f * S, home.y, home.z};
        const Vec3 stuck = st.clampToResident(stranded, target);
        check(stuck.x == stranded.x, "a mover already in unloaded space is left where it is");

        st.unloadAll(w, bodies, freed);
        w.flush();
    }

    // ---- several actors at once ----------------------------------------------------------------------
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 2;
        s.evictRadius = 4;
        s.verticalRadius = 0;
        s.loadBudget = 0;
        s.evictBudget = 0;
        s.leadSeconds = 1.0f;
        st.setSettings(s);
        st.setSource(&src);

        // Three actors, far apart, moving in different directions. The camera is just one of them.
        const std::vector<StreamSource> actors = {
            StreamSource{Vec3{0, 0, 0},               Vec3{6000, 0, 0}},
            StreamSource{Vec3{50.0f * S, 0, 0},       Vec3{0, -6000, 0}},
            StreamSource{Vec3{0, -50.0f * S, 0},      Vec3{0, 0, 0}},
        };
        st.setSources(actors);
        std::vector<i32> freed;
        st.update(w, bodies, &freed);
        w.flush();

        u32 covered = 0;
        for (const StreamSource& a : actors) if (st.isResidentAt(a.positionCm)) ++covered;
        check(covered == actors.size(), "every actor has the ground under it loaded, not just the first");

        u32 ledFor = 0;
        for (const StreamSource& a : actors) {
            if (a.velocityCmPerSec.x == 0 && a.velocityCmPerSec.y == 0) continue;
            const Vec3 ahead{a.positionCm.x + a.velocityCmPerSec.x * s.leadSeconds,
                             a.positionCm.y + a.velocityCmPerSec.y * s.leadSeconds, a.positionCm.z};
            if (st.isResidentAt(ahead)) ++ledFor;
        }
        check(ledFor == 2, "and each MOVING actor has the space it is heading into loaded too");
        AVER_INFO("   note  three actors: {} chunks resident across three separate neighbourhoods",
                  st.residentCount());

        st.unloadAll(w, bodies, freed);
        w.flush();
        check(w.count() == 0, "everything unloads cleanly");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
