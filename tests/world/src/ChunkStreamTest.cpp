// Streaming residency: a camera flight, a bounded resident set, and CHUNKED EQUALS FLAT.
// Exit code = failure count.
//
// docs/CHUNKS.md section 10 calls chunked-equals-flat "the strongest single test" in the streaming
// plan, and it is the reason section 3.1 keeps flat loading permanently rather than until migration
// finishes: delete the flat path and you are left comparing streaming against itself.
//
// The whole thing is headless. The scene is real, the region files are real, and the only thing
// standing in for the engine is the camera, which is a Vec3.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkPartition.hpp"
#include "aver/world/ChunkSource.hpp"
#include "aver/world/ChunkStreamer.hpp"
#include "aver/world/RegionFile.hpp"
#include "aver/world/RegionIndex.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;
static std::string why;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
static void checkWhy(bool cond, const std::string& what) { check(cond, cond ? what : what + ": " + why); }

// name -> world position and surface, which is what "the same entity set" means here. Handles
// differ between a flat load and a streamed one and are not part of the comparison.
struct Flat {
    std::map<std::string, std::array<f32, 3>> pos;
    std::map<std::string, std::string> surface;
};

static Flat snapshotFlat(const scene::World& w) {
    Flat f;
    for (u32 i = 0; i < w.count(); ++i) {
        const scene::Entity e = w.at(i);
        if (!w.valid(e) || w.destroyPending(e)) continue;
        const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
        if (!loc) continue;
        const std::string n = w.name(e);
        f.pos[n] = {loc->xf.position.x, loc->xf.position.y, loc->xf.position.z};
        std::string s;
        if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer))
            if (mr->material) s = aver_scene_material_name(mr->material);
        f.surface[n] = s;
    }
    return f;
}

// Builds a wide flat world: a grid of entities spread over several regions, so a flight really does
// cross region files rather than wandering inside one.
static std::vector<scene::Entity> buildFlatWorld(scene::World& w, i32 chunkSizeCm) {
    std::vector<scene::Entity> roots;
    // 40 x 3 chunks of content. At 16 m that is 640 m x 48 m -- and with regions 1024 chunks wide,
    // deliberately NOT enough to cross a region, so the flight below uses a big stride instead.
    for (i32 cx = -20; cx < 20; ++cx) {
        for (i32 cy = -1; cy <= 1; ++cy) {
            const f32 x = static_cast<f32>(cx) * chunkSizeCm + 800.0f;   // mid-chunk
            const f32 y = static_cast<f32>(cy) * chunkSizeCm + 800.0f;
            Transform xf;
            xf.position = Vec3{x, y, 0.0f};
            const std::string name = "tile_" + std::to_string(cx) + "_" + std::to_string(cy);
            const scene::Entity e = w.create(name, scene::kInvalidEntity, xf);
            if (e == scene::kInvalidEntity) continue;
            auto* mr = static_cast<scene::CMeshRenderer*>(
                w.addComponent(e, scene::kComponentMeshRenderer));
            if (mr) {
                mr->mesh = 0x5555ull + static_cast<u64>(cx * 31 + cy);
                mr->material = aver_scene_material(0, (cx % 2) ? "M_Wall" : "M_Floor");
                mr->flags = scene::kMeshRendererVisible;
            }
            roots.push_back(e);

            // Every fourth tile carries a child, so hierarchies cross the streaming path too.
            if (cx % 4 == 0) {
                Transform cxf;
                cxf.position = Vec3{50.0f, 60.0f, 70.0f};
                w.create(name + "_child", e, cxf);
            }
        }
    }

    // ...and a cluster in each of NINE regions, because slice 6's DONE-WHEN asks for a flight across
    // at least that many and the strip above spans exactly one: a region is 1024 chunks, which at
    // 16 m is 16.384 km, so 640 m of content never leaves the middle of region (0,0,0).
    //
    // These sit at 0 and +-16.384 km. Past the sub-millimetre budget (2^20 cm = 10.49 km, see
    // ChunkCoord.hpp) and deliberately so -- that is the regime slice 10's floating origin exists
    // for. Whole centimetres out there are still exact, being under f32's 2^24 integer limit, so
    // this measures the streaming and not f32's rounding.
    for (i32 rx = -1; rx <= 1; ++rx) {
        for (i32 ry = -1; ry <= 1; ++ry) {
            const f64 ox = static_cast<f64>(rx) * kChunksPerRegionAxis * chunkSizeCm;
            const f64 oy = static_cast<f64>(ry) * kChunksPerRegionAxis * chunkSizeCm;
            for (int k = 0; k < 3; ++k) {
                Transform xf;
                xf.position = Vec3{static_cast<f32>(ox + k * chunkSizeCm + 800.0),
                                   static_cast<f32>(oy + 800.0), 0.0f};
                const std::string name = "far_" + std::to_string(rx) + "_" + std::to_string(ry) +
                                         "_" + std::to_string(k);
                const scene::Entity e = w.create(name, scene::kInvalidEntity, xf);
                if (e == scene::kInvalidEntity) continue;
                auto* mr = static_cast<scene::CMeshRenderer*>(
                    w.addComponent(e, scene::kComponentMeshRenderer));
                if (mr) {
                    mr->mesh = 0x7777ull + static_cast<u64>(rx * 7 + ry * 3 + k);
                    mr->material = aver_scene_material(0, "M_Accent");
                    mr->flags = scene::kMeshRendererVisible;
                }
                roots.push_back(e);
            }
        }
    }
    return roots;
}

// The world-space centre of a region's content cluster, matching buildFlatWorld above.
static Vec3 regionClusterAt(i32 rx, i32 ry, i32 chunkSizeCm) {
    return Vec3{static_cast<f32>(static_cast<f64>(rx) * kChunksPerRegionAxis * chunkSizeCm + 800.0),
                static_cast<f32>(static_cast<f64>(ry) * kChunksPerRegionAxis * chunkSizeCm + 800.0),
                0.0f};
}

int main() {
    const i32 S = kDefaultChunkSizeCm;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-stream-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    scene::World& w = scene::World::instance();

    // ---- build flat, snapshot it, cook it -----------------------------------------------------------
    const std::vector<scene::Entity> flatRoots = buildFlatWorld(w, S);
    check(!flatRoots.empty(), "the flat world was built");
    const Flat flat = snapshotFlat(w);
    const u32 flatCount = w.count();
    AVER_INFO("   note  flat world: {} entities across {} roots", flatCount, flatRoots.size());

    PartitionOptions po;
    po.chunkSizeCm = S;
    const Partition part = partitionWorld(w, po);
    const auto payloads = captureAll(w, part, S);
    check(!payloads.empty(), "it partitions into chunks");

    // Group the chunks by region and cook one file each.
    std::map<std::string, std::vector<std::pair<ChunkLocal, ChunkPayload>>> byRegion;
    std::map<std::string, RegionCoord> regionOfKey;
    for (const auto& kv : payloads) {
        const RegionCoord r = regionOf(kv.first);
        const std::string key = std::to_string(r.x) + "." + std::to_string(r.y) + "." + std::to_string(r.z);
        byRegion[key].emplace_back(localOf(kv.first), kv.second);
        regionOfKey[key] = r;
    }
    AVER_INFO("   note  the content spans {} region file(s)", byRegion.size());
    check(byRegion.size() >= 9, "the fixture spans at least nine regions, as slice 6's DONE-WHEN asks");

    RegionIndex ix;
    ix.levelId = 0x1234;
    ix.chunkSizeCm = S;
    for (auto& kv : byRegion) {
        const std::string rel = "r." + kv.first + ".avrgn";
        RegionWriteDesc d;
        d.coord = regionOfKey[kv.first];
        d.chunkSizeCm = S;
        d.levelId = ix.levelId;
        checkWhy(writeRegion((dir / rel).string(), d, kv.second, &why), "region " + kv.first + " cooks");

        RegionFile rf;
        rf.open((dir / rel).string(), &why);
        RegionEntry e;
        e.coord = d.coord;
        e.contentHash = rf.header().contentHash;
        e.chunkCount = rf.header().chunkCount;
        e.relativePath = rel;
        ix.add(e);
    }
    const std::string indexPath = (dir / "level.ocindex").string();
    checkWhy(writeIndex(indexPath, ix, &why), "the index writes");

    // Tear the flat world down. Everything after this compares against the SNAPSHOT, so nothing can
    // accidentally be comparing a streamed world against surviving flat entities.
    for (const scene::Entity e : flatRoots) w.destroy(e);
    w.flush();
    check(w.count() == 0, "the flat world is gone");

    RegionChunkSource src;
    checkWhy(src.open(indexPath, dir.string(), &why), "the streaming source opens");
    check(src.chunkSizeCm() == S, "and takes its chunk size from the index");

    // ---- CHUNKED EQUALS FLAT ---------------------------------------------------------------------
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 64;        // far wider than the content
        s.evictRadius = 128;
        s.verticalRadius = 4;
        s.loadBudget = 0;         // unbounded: this test is about equality, not pacing
        st.setSettings(s);
        st.setSource(&src);
        // One source per region cluster: a 64-chunk radius is 1 km and the clusters are 16 km apart,
        // so a single source could never reach them. This also exercises the multi-source path,
        // which is what an actor becomes in slice 9.
        std::vector<Vec3> all;
        for (i32 rx = -1; rx <= 1; ++rx)
            for (i32 ry = -1; ry <= 1; ++ry) all.push_back(regionClusterAt(rx, ry, S));
        all.push_back(Vec3{0, 0, 0});
        st.setSources(all);

        const StreamStats stats = st.update(w, bodies);
        check(stats.failedLoads == 0, "nothing failed to load");
        check(w.count() == flatCount, "the streamed world has exactly as many entities as the flat one");

        const Flat streamed = snapshotFlat(w);
        check(streamed.pos.size() == flat.pos.size(), "the same number of distinct names");

        u32 missing = 0, moved = 0, wrongSurface = 0;
        for (const auto& kv : flat.pos) {
            const auto it = streamed.pos.find(kv.first);
            if (it == streamed.pos.end()) { ++missing; continue; }
            if (it->second != kv.second) ++moved;
            if (streamed.surface.at(kv.first) != flat.surface.at(kv.first)) ++wrongSurface;
        }
        u32 invented = 0;
        for (const auto& kv : streamed.pos) if (!flat.pos.count(kv.first)) ++invented;

        check(missing == 0, "no entity is missing from the streamed world");
        check(invented == 0, "and none was invented");
        check(moved == 0, "CHUNKED EQUALS FLAT: every world position is bit-identical");
        check(wrongSurface == 0, "...and every surface name too");

        std::vector<i32> freed;
        st.unloadAll(w, bodies, freed);
        w.flush();
        check(w.count() == 0, "unloadAll empties the world");
    }

    // ---- a camera flight -------------------------------------------------------------------------
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 3;
        s.evictRadius = 5;
        s.verticalRadius = 1;
        s.loadBudget = 4;
        s.evictBudget = 8;
        st.setSettings(s);
        st.setSource(&src);

        // The wanted set is at most (2*3+1)^2 * (2*1+1) = 147 chunks; the resident set may exceed it
        // transiently by whatever the eviction budget has not caught up with.
        const u32 wantedMax = 7u * 7u * 3u;

        u32 peakChunks = 0, peakEntities = 0, totalLoaded = 0, totalEvicted = 0;
        bool everNegative = false;
        // West to east across the whole 640 m of content, then back -- so chunks are loaded, left
        // behind, and then wanted a second time.
        for (int step = 0; step <= 80; ++step) {
            const int at = step <= 40 ? step : 80 - step;
            const f32 x = static_cast<f32>(at - 20) * S + 800.0f;
            st.setSources({Vec3{x, 800.0f, 0.0f}});
            std::vector<i32> freed;
            const StreamStats stats = st.update(w, bodies, &freed);
            peakChunks = std::max(peakChunks, stats.residentChunks);
            peakEntities = std::max(peakEntities, stats.residentEntities);
            totalLoaded += stats.loadedThisUpdate;
            totalEvicted += stats.evictedThisUpdate;
            if (stats.residentEntities > flatCount) everNegative = true;
            w.flush();   // the frame boundary: deferred destroys actually retire here
        }

        check(totalLoaded > 0, "the flight loaded chunks");
        check(totalEvicted > 0, "...and evicted them again, so residency really did turn over");
        check(!everNegative, "the resident entity count never exceeded the whole world");
        check(peakChunks <= wantedMax + s.evictBudget + 4,
              "the resident set stayed BOUNDED across the flight");
        AVER_INFO("   note  flight: {} loads, {} evictions, peak {} chunks / {} entities (wanted set {})",
                  totalLoaded, totalEvicted, peakChunks, peakEntities, wantedMax);

        // THE MEASUREMENT docs/CHUNKS.md slice 6 asks to be written down.
        const StreamStats& f = st.stats();
        check(f.totalLoads > 0, "load timings were collected");
        if (f.totalLoads)
            AVER_INFO("   note  chunk materialisation: {} loads, {:.4f} ms mean, {:.4f} ms last",
                      f.totalLoads, f.totalLoadMs / static_cast<f64>(f.totalLoads), f.lastLoadMs);

        // Slot churn, which docs/CHUNKS.md B7 says is what threatens a long session: 24 index bits
        // and only 7 generation bits, and a retired slot is never reissued.
        AVER_INFO("   note  entity slots after the flight: {} retired, {} free",
                  w.retiredSlotCount(), w.freeSlotCount());
        check(w.retiredSlotCount() == 0, "no entity slot was exhausted by this flight");

        std::vector<i32> freed;
        st.unloadAll(w, bodies, freed);
        w.flush();
    }

    // ---- a flight ACROSS NINE REGIONS -----------------------------------------------------------------
    //
    // The strip flight above stays inside one region file. This one visits all nine clusters in turn,
    // so the source has to open, cache and retire region FILES -- and its open-handle cache is capped
    // at 8, which nine regions is deliberately one more than.
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 3;
        s.evictRadius = 5;
        s.verticalRadius = 1;
        s.loadBudget = 0;      // arrive and settle in one step, so each stop is measured cleanly
        s.evictBudget = 0;
        st.setSettings(s);
        st.setSource(&src);

        u32 visitedWithContent = 0, peak = 0;
        std::vector<RegionCoord> seen;
        for (i32 rx = -1; rx <= 1; ++rx) {
            for (i32 ry = -1; ry <= 1; ++ry) {
                const Vec3 at = regionClusterAt(rx, ry, S);
                st.setSources({at});
                std::vector<i32> freed;
                const StreamStats stats = st.update(w, bodies, &freed);
                w.flush();
                peak = std::max(peak, stats.residentEntities);
                // Each cluster is three tiles, all within the load radius of its own centre.
                if (stats.residentEntities >= 3) ++visitedWithContent;
                seen.push_back(regionOf(splitCm(at, S).chunk));
            }
        }
        check(visitedWithContent == 9, "all NINE regions delivered their content when flown to");
        std::sort(seen.begin(), seen.end(), [](const RegionCoord& a, const RegionCoord& b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        });
        seen.erase(std::unique(seen.begin(), seen.end(), [](const RegionCoord& a, const RegionCoord& b) {
            return a == b;
        }), seen.end());
        check(seen.size() == 9, "...and they really were nine DISTINCT regions, not one revisited");
        check(src.openRegions() <= 8, "the source's open-file cache stayed within its cap");
        AVER_INFO("   note  nine-region flight: peak {} resident entities, {} region files held open",
                  peak, src.openRegions());

        std::vector<i32> freed;
        st.unloadAll(w, bodies, freed);
        w.flush();
        check(w.count() == 0, "the nine-region flight unloads cleanly");
    }

    // ---- hysteresis, which is what stops boundary thrash --------------------------------------------
    {
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.chunkSizeCm = S;
        s.loadRadius = 2;
        s.evictRadius = 2;          // deliberately equal: setSettings must refuse this
        st.setSettings(s);
        check(st.settings().evictRadius > st.settings().loadRadius,
              "an evictRadius equal to loadRadius is RAISED, not accepted -- it is the thrash case");

        s.evictRadius = 4;
        s.loadBudget = 0;
        st.setSettings(s);
        st.setSource(&src);

        // Sit exactly on a chunk boundary and jitter across it. With hysteresis, nothing should be
        // evicted at all: the boundary is well inside the evict radius.
        u32 churn = 0;
        for (int i = 0; i < 12; ++i) {
            const f32 x = (i % 2) ? -0.5f : 0.5f;    // straddling the chunk 0 / chunk -1 line
            st.setSources({Vec3{x, 0.0f, 0.0f}});
            std::vector<i32> freed;
            const StreamStats stats = st.update(w, bodies, &freed);
            if (i > 1) churn += stats.loadedThisUpdate + stats.evictedThisUpdate;
            w.flush();
        }
        check(churn == 0, "jittering across a chunk boundary causes NO load/evict churn once settled");

        std::vector<i32> freed;
        st.unloadAll(w, bodies, freed);
        w.flush();
    }

    // ---- a stale index is refused rather than served ---------------------------------------------
    {
        // Rewrite one region's recorded hash so it disagrees with the file, exactly as an index that
        // was not regenerated after a re-cook would.
        RegionIndex bad = ix;
        bad.regions[0].contentHash ^= 0xFFull;
        const std::string p = (dir / "stale.ocindex").string();
        writeIndex(p, bad, &why);

        RegionChunkSource s2;
        checkWhy(s2.open(p, dir.string(), &why), "a stale index still OPENS -- it is not itself corrupt");
        ChunkPayload out;
        // Some chunk in that region must now be refused rather than served.
        RegionFile probe;
        probe.open((dir / ("r." + std::to_string(bad.regions[0].coord.x) + "." +
                           std::to_string(bad.regions[0].coord.y) + "." +
                           std::to_string(bad.regions[0].coord.z) + ".avrgn")).string(), &why);
        const std::vector<ChunkLocal> inThere = probe.chunks();
        check(!inThere.empty(), "the region it names has chunks");
        if (!inThere.empty()) {
            const ChunkCoord c = chunkOf(bad.regions[0].coord, inThere.front());
            std::string w2;
            check(!s2.load(c, out, &w2), "a chunk from a region the index disagrees with is REFUSED");
            check(w2.find("stale") != std::string::npos, "...and says the index is stale: " + w2);
        }
    }

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
