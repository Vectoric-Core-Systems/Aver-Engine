// ChunkWorld -- the bound-together generator+override+streamer object, driven the way a host
// actually would: one call a frame with "the viewer is here, moving like this."
// Exit code = failure count.
//
// THIS IS NOT A RE-RUN OF ChunkActorStreamTest. That test proves the STREAMER's residency and
// boundary-hold properties directly against a hand-wired GeneratedChunkSource. This test proves
// ChunkWorld does not lose or misrepresent any of that once it is the thing assembling the pieces --
// that a viewer flight through a ChunkWorld still loads ahead and evicts behind, that
// streamedEntities()/owns() agree with the streamer's own resident-entity count at every step (the
// property that did not exist before residentChunks() gave anything a way to enumerate it), and that
// a world directory that does not exist yet is CREATED rather than refused.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/ChunkWorld.hpp"

#include <filesystem>
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
static void checkWhy(bool cond, const std::string& what, const std::string& why) {
    check(cond, cond ? what : what + ": " + why);
}

// Every surface chunk has something, and payloads are small -- this test is about residency moving
// with the viewer, not about content, exactly the same choice ChunkActorStreamTest makes and for the
// same reason.
static GeneratorSettings genSettings() {
    GeneratorSettings g;
    g.worldSeed = 0xC0DE'C0DE'0002ull;
    g.samplesPerAxis = 2;
    g.threshold = 0.0f;
    g.featureSizeCm = 1600.0f;
    return g;
}

int main() {
    const i32 S = kDefaultChunkSizeCm;
    scene::World& w = scene::World::instance();

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "aver-chunkworld-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);   // start clean; also proves open() below CREATES it

    check(!directoryExists(dir.string()), "the world directory does not exist before open()");

    ChunkWorldSettings ws;
    ws.worldDir = dir.string();
    ws.generator = genSettings();
    ws.stream.chunkSizeCm = S;
    ws.stream.loadRadius = 3;
    ws.stream.evictRadius = 5;
    ws.stream.verticalRadius = 0;
    ws.stream.loadBudget = 8;
    ws.stream.evictBudget = 8;
    ws.stream.leadSeconds = 1.5f;

    ChunkWorld world;
    std::string why;
    checkWhy(world.open(ws, &why), "ChunkWorld::open succeeds on a directory that did not exist yet", why);
    check(directoryExists(dir.string()), "...and the directory now exists, having been created rather than refused");

    // ---- nothing resident, nothing owned, before the first update -----------------------------------
    check(world.streamedEntities().empty(), "before any update, ChunkWorld owns nothing");
    check(world.stats().residentChunks == 0, "...and the streamer agrees: zero chunks resident");

    // ---- warm-up: fill the neighbourhood before asking anything of a moving viewer ------------------
    // Exactly ChunkActorStreamTest's reasoning: nothing is resident on frame zero, and a fast viewer
    // in the same frame as spawn is not a case any budget can serve.
    const f32 dt = 1.0f / 60.0f;
    const f32 speed = 60000.0f;   // 600 m/s, same speed ChunkActorStreamTest picks to actually stress the lead
    Vec3 pos{0, 0, 0};
    const Vec3 vel{speed, 0, 0};

    u32 warmUpdates = 0;
    for (int i = 0; i < 200; ++i) {
        const StreamStats s = world.update(w, pos, vel, dt);
        ++warmUpdates;
        if (s.pendingLoads == 0) break;
    }
    check(world.streamer().isResidentAt(pos), "after warm-up the viewer's own chunk is loaded");
    AVER_INFO("   note  warm-up took {} updates before the wanted set was satisfied", warmUpdates);

    // ---- entity counts move with residency: checked EVERY step, not just at the end -----------------
    // If ChunkWorld's owned list ever drifted from what the streamer actually holds -- a stale entry
    // left behind by an eviction, or a load that updated the streamer but not ChunkWorld's view of
    // it -- this is where it would show up, on the very frame it happened rather than averaged away
    // over a whole flight.
    u32 steps = 0, arrivedUnloaded = 0, countMismatches = 0;
    ChunkCoord originChunk{0, 0, 0};
    for (int i = 0; i < 600; ++i) {   // ten seconds, 6 km, same shape as ChunkActorStreamTest's flight
        std::vector<i32> freed;
        const StreamStats s = world.update(w, pos, vel, dt, &freed);

        if (world.streamedEntities().size() != s.residentEntities) ++countMismatches;
        // owns() must agree with membership in the very list it is built from.
        if (!world.streamedEntities().empty() && !world.owns(world.streamedEntities().front()))
            ++countMismatches;
        if (world.owns(scene::kInvalidEntity))
            ++countMismatches;   // an id nothing ever created must never read as owned

        const Vec3 next{pos.x + vel.x * dt, pos.y, pos.z};
        if (!world.streamer().isResidentAt(next)) ++arrivedUnloaded;
        pos = next;
        ++steps;
    }
    check(steps == 600, "the flight ran to completion");
    check(countMismatches == 0,
          "streamedEntities().size() equalled the streamer's own residentEntities stat on EVERY step");
    check(arrivedUnloaded == 0,
          "at 600 m/s the viewer never arrives in an unloaded chunk -- ChunkWorld carries the streamer's "
          "own lead-ahead property through");
    AVER_INFO("   note  {} steps, {} arrivals in unloaded space, {} chunks / {} entities resident at the end",
              steps, arrivedUnloaded, world.stats().residentChunks, world.stats().residentEntities);

    // ---- loaded ahead, evicted behind -----------------------------------------------------------------
    // The viewer travelled 6 km at 16 m/chunk -- 375 chunks -- with evictRadius = 5. The chunk it
    // started in is now more than 300 chunks behind; it must be long gone, and something well ahead
    // of the viewer's CURRENT position must be resident instead.
    check(!world.streamer().isResident(originChunk),
          "the origin chunk, left far behind, has been evicted -- not left resident forever");
    const Vec3 ahead{pos.x + 4.0f * S, pos.y, pos.z};
    check(world.streamer().isResidentAt(ahead),
          "space ahead of the viewer's current position is resident -- the world streams AHEAD, not just AT");

    // ---- shutdown leaves the scene as it found it -------------------------------------------------
    const u32 beforeShutdown = w.count();
    check(beforeShutdown > 0, "the scene is non-empty before shutdown (or the rest of this test proved nothing)");
    std::vector<i32> freedAtShutdown;
    world.shutdown(w, freedAtShutdown);
    check(w.count() == 0, "shutdown leaves the scene EMPTY -- every streamed entity, not just the recent ones");
    check(world.streamedEntities().empty(), "...and ChunkWorld's own view agrees: it owns nothing afterward");
    check(world.stats().residentChunks == 0 && world.stats().residentEntities == 0,
          "...and the streamer's own stats fall back to zero, not a stale snapshot from before shutdown");

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
