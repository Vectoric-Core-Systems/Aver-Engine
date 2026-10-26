// Streaming a cooked authored level (LARGE_WORLD 2.4.6, tests 5.1 AuthoredStreamTest). Exit code = failure count.
//
// The streamer upgrades the authored path needs, each with a CONTROL that shows the property is the upgrade's and not an accident:
//   COLUMNS       with kAllLayers a tower whose floors span chunk layers -1..31 is found; the cube scan of the same source holds three
//   PAD           an owner chunk padChunks beyond the load radius is loaded; with no pad it is not
//   WIDE          a bridge's owner chunk 20 chunks away loads while the box is near, is kept through the hysteresis, and goes when
//                 the box is out of reach; without the list it never loads
//   PREPARE GATE  a host that says Pending holds a chunk back (decoded once, retried every update, dropped when no longer wanted);
//                 Failed leaves it resident and empty and is not retried
//   BUDGETS       a millisecond load budget loads at least one and at most a few chunks per update and still finishes; an entity cap
//                 stops at the chunk that crosses it; a millisecond evict budget does the same for evictions
//   PIN           a pinned chunk loads first, wherever the sources are, and survives until it is unpinned
//   SCATTER HOST  the default host is today's behaviour: a flight with no host and with ScatterChunkHost gives identical stats
//   OCCUPANCY     RegionChunkSource::forEachOccupied equals a has() scan of the same box, packed and version 2, across a region seam
//   CHUNKED = FLAT  a cooked level streamed back equals world::instantiate of the same level, entity for entity, at origin zero and
//                 at an origin 61 regions out (positions on a 0.25 cm grid, so the comparison is bit for bit)
// No GPU, no window, no .NET.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/AuthoredChunkSource.hpp"
#include "aver/world/ChunkCook.hpp"
#include "aver/world/ChunkHost.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"
#include "aver/world/ChunkStreamer.hpp"
#include "aver/world/ChunkWorld.hpp"
#include "aver/world/LevelAssetLibrary.hpp"
#include "aver/world/LevelInstance.hpp"
#include "aver/world/LevelTransform.hpp"
#include "aver/world/RegionFile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

static constexpr i32 S = kDefaultChunkSizeCm;

// The centre of a chunk, in the scene frame at origin zero.
static Vec3 centreOf(i32 cx, i32 cy, i32 cz) {
    return Vec3{static_cast<f32>(cx * S + S / 2), static_cast<f32>(cy * S + S / 2), static_cast<f32>(cz * S + S / 2)};
}

static void spinMs(f64 ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() < ms) {}
}

// ---- sources and hosts ---------------------------------------------------------------------------------------------------------

// An explicit set of occupied chunks; every one holds `perChunk` plain entities.
class MapSource final : public IChunkSource {
public:
    std::set<std::tuple<i32, i32, i32>> chunks;
    u32 perChunk = 4;
    std::unordered_map<ChunkCoord, u32> loads;   // per-chunk load() calls

    void add(i32 x, i32 y, i32 z) { chunks.insert({x, y, z}); }
    bool has(const ChunkCoord& c) override { return chunks.count({c.x, c.y, c.z}) != 0; }
    bool load(const ChunkCoord& c, ChunkPayload& out, std::string* err) override {
        if (!has(c)) { if (err) *err = "no such chunk"; return false; }
        ++loads[c];
        out = ChunkPayload{};
        out.coord = c;
        out.chunkSizeCm = S;
        for (u32 i = 0; i < perChunk; ++i) {
            PayloadEntity e;
            e.name = "m_" + std::to_string(c.x) + "_" + std::to_string(c.y) + "_" + std::to_string(c.z) + "_" + std::to_string(i);
            e.objectId = static_cast<u64>(i) + 1;
            e.local.position = Vec3{100.0f + 10.0f * static_cast<f32>(i), 200.0f, 300.0f};
            out.entities.push_back(std::move(e));
        }
        return true;
    }
    void forEachOccupied(const ChunkBox& box, const std::function<void(const ChunkCoord&)>& fn) override {
        for (const auto& k : chunks) {
            const ChunkCoord c{std::get<0>(k), std::get<1>(k), std::get<2>(k)};
            if (c.x < box.min.x || c.x > box.max.x || c.y < box.min.y || c.y > box.max.y || c.z < box.min.z || c.z > box.max.z) continue;
            fn(c);
        }
    }
};

// A host that can hold a chunk back, refuse one, and take its time.
class TestHost final : public IChunkHost {
public:
    std::unordered_map<ChunkCoord, int> pendingFor;   // prepare() answers Pending this many times
    std::unordered_set<ChunkCoord> failFor;
    std::unordered_map<ChunkCoord, u32> prepareCalls, restoreCalls, evictCalls;
    f64 restoreSpinMs = 0.0, evictSpinMs = 0.0;
    ScatterChunkHost inner;

    ChunkPrepare prepare(const ChunkPayload& p) override {
        ++prepareCalls[p.coord];
        if (failFor.count(p.coord)) return ChunkPrepare::Failed;
        const auto it = pendingFor.find(p.coord);
        if (it != pendingFor.end() && it->second > 0) { --it->second; return ChunkPrepare::Pending; }
        return ChunkPrepare::Ready;
    }
    std::vector<scene::Entity> restore(const ChunkPayload& p, scene::World& w, const RestoreOptions& opt) override {
        ++restoreCalls[p.coord];
        spinMs(restoreSpinMs);
        return inner.restore(p, w, opt);
    }
    u32 evict(const ChunkCoord& c, const std::vector<scene::Entity>& es, scene::World& w, BodyRegistry& bodies,
              std::vector<i32>& freed) override {
        ++evictCalls[c];
        spinMs(evictSpinMs);
        return inner.evict(c, es, w, bodies, freed);
    }
};

struct Rig {
    ChunkStreamer st;
    BodyRegistry bodies;
    std::vector<i32> freed;
    scene::World& w = scene::World::instance();

    StreamStats step(const Vec3& pos, OriginCm o = {}) {
        st.setSources(std::vector<Vec3>{pos});
        const StreamStats s = st.update(w, bodies, &freed, o);
        w.flush();
        return s;
    }
    // Updates until nothing is left pending.
    StreamStats settle(const Vec3& pos, int maxSteps = 300) {
        StreamStats s;
        for (int i = 0; i < maxSteps; ++i) {
            s = step(pos);
            if (s.pendingLoads == 0) break;
        }
        return s;
    }
    void clear() {
        st.unloadAll(w, bodies, freed);
        w.flush();
    }
};

static StreamSettings plain(i32 load, i32 evict) {
    StreamSettings s;
    s.loadRadius = load;
    s.evictRadius = evict;
    s.loadBudget = 0;
    s.evictBudget = 0;
    s.leadSeconds = 0.0f;
    return s;
}

static fmt::OcWorldPlacement place(const std::string& asset, f64 x, f64 y, f64 z, u64 id) {
    fmt::OcWorldPlacement p;
    p.asset = asset;
    p.objectId = fnv1a64(std::string_view(asset));
    p.x = x; p.y = y; p.z = z;
    p.placementId = id;
    return p;
}

static CookInput cookInput(LevelAssetLibrary* lib, const std::string& projectDir) {
    CookInput in;
    in.levelAsset = "Maps/Stream.ocworld";
    in.levelStem = "Stream";
    in.projectDir = projectDir;
    in.library = lib;
    in.meshBounds = [](const std::string& asset, Vec3& lo, Vec3& hi) {
        if (asset.rfind("Meshes/P", 0) == 0 || asset == "Meshes/Box.ocmesh") { lo = Vec3{-100, -100, -100}; hi = Vec3{100, 100, 100}; return true; }
        return false;
    };
    return in;
}

static bool cookTo(const fmt::OcWorldData& level, const std::filesystem::path& dir, CookReport* repOut = nullptr) {
    LevelAssetLibrary lib;
    lib.setLoader([](const std::string&, fmt::OcWorldData&, std::string* err) { if (err) *err = "no children in this test"; return false; });
    CookInput in = cookInput(&lib, dir.string());
    CookOptions opt;
    opt.outDir = dir.string();
    CookReport rep;
    const bool ok = cookAuthoredLevel(level, in, opt, rep, &why);
    if (repOut) *repOut = rep;
    return ok;
}

// ---- COLUMNS --------------------------------------------------------------------------------------------------------------------

static void columnChecks(const std::filesystem::path& root) {
    AVER_INFO("-- columns");
    namespace fs = std::filesystem;
    fmt::OcWorldData level;
    level.name = "Tower";
    for (int layer = -1; layer <= 31; ++layer)
        level.placements.push_back(place("Meshes/Box.ocmesh", 800, 800, layer * S + 800, 0x1000 + static_cast<u64>(layer + 1)));
    level.placements.push_back(place("Meshes/Box.ocmesh", 3000, 3000, 0, 0x2000));
    const fs::path dir = root / "tower";
    checkWhy(cookTo(level, dir), "the tower cooks");

    scene::World& w = scene::World::instance();
    StreamSettings ss = authoredStreamSettings(S, 4800.0f, 8000.0f);
    ss.loadBudgetMs = ss.loadBudgetFarMs = ss.evictBudgetMs = 0.0;
    ss.loadEntityCap = 0;
    ss.leadSeconds = 0.0f;

    ChunkWorld cw;
    checkWhy(cw.openAuthored(dir.string(), ss, nullptr, &why), "ChunkWorld::openAuthored opens the cooked set");
    check(cw.authored() && cw.streamer().settings().verticalRadius == kAllLayers && cw.streamer().settings().padChunks >= kAuthoredPadChunks,
          "...streaming whole columns with at least the fixed pad");
    std::vector<i32> freed;
    StreamStats s;
    for (int i = 0; i < 20; ++i) { s = cw.update(w, centreOf(0, 0, 0), Vec3{0, 0, 0}, 0.016f, &freed); if (s.pendingLoads == 0) break; }
    bool allTower = true;
    for (int layer = -1; layer <= 31; ++layer) allTower = allTower && cw.streamer().isResident(ChunkCoord{0, 0, layer});
    check(allTower, "every tower chunk, layers -1 to 31, is resident with the camera on layer 0");
    check(s.residentChunks == 34 && s.residentEntities == 34 && cw.streamedEntities().size() == 34, "34 chunks, 34 entities (33 floors + the ground box)");

    // Up at the top of the tower nothing changes: the column is found by XY alone.
    for (int i = 0; i < 4; ++i) s = cw.update(w, centreOf(0, 0, 31), Vec3{0, 0, 0}, 0.016f, &freed);
    check(s.residentChunks == 34 && s.loadedThisUpdate == 0 && s.evictedThisUpdate == 0, "...and nothing loads or evicts when the camera climbs to layer 31");

    // CONTROL: the cube scan (verticalRadius 1) of the same source holds only the layers next to the camera.
    {
        AuthoredChunkSource src;
        checkWhy(src.open(dir.string(), &why), "the control opens the same set");
        Rig rig;
        StreamSettings cs = plain(3, 5);
        cs.verticalRadius = 1;
        rig.st.setSettings(cs);
        rig.st.setSource(&src);
        rig.settle(centreOf(0, 0, 0));
        check(rig.st.isResident(ChunkCoord{0, 0, 0}) && rig.st.isResident(ChunkCoord{0, 0, 1}) && !rig.st.isResident(ChunkCoord{0, 0, 31}) &&
                  rig.st.residentCount() == 4,
              "CONTROL: with verticalRadius 1 only layers -1..1 of the tower (and the ground box) are resident");
        rig.clear();
    }
    cw.shutdown(w, freed);
    check(cw.streamedEntities().empty(), "shutdown releases the scene");

    // Not a cooked set.
    {
        ChunkWorld bad;
        check(!bad.openAuthored((root / "nowhere").string(), ss, nullptr, &why), "a directory with no cooked index is refused");
        const fs::path overrides = root / "overrides";
        std::error_code ec;
        fs::create_directories(overrides, ec);
        RegionIndex ix;   // kind 0: persisted overrides
        writeIndex((overrides / kCookIndexName).string(), ix, nullptr);
        why.clear();
        check(!bad.openAuthored(overrides.string(), ss, nullptr, &why) && why.find("overrides") != std::string::npos,
              "an overrides index is refused by name: " + why);
    }
}

// ---- PAD / WIDE -----------------------------------------------------------------------------------------------------------------

static void padChecks() {
    AVER_INFO("-- pad");
    MapSource src;
    for (int x = 0; x <= 10; ++x) src.add(x, 0, 0);
    for (int pad : {0, 2}) {
        Rig rig;
        StreamSettings s = plain(3, 5);
        s.verticalRadius = kAllLayers;
        s.padChunks = pad;
        rig.st.setSettings(s);
        rig.st.setSource(&src);
        rig.settle(centreOf(0, 0, 0));
        const i32 want = 3 + pad;
        bool edge = rig.st.isResident(ChunkCoord{want, 0, 0});
        bool beyond = rig.st.isResident(ChunkCoord{want + 1, 0, 0});
        check(edge && !beyond, std::string(pad ? "pad 2: the owner chunk 5 away is resident and the one 6 away is not"
                                               : "CONTROL, pad 0: the chunk 3 away is resident and the one 4 away is not"));
        // Hysteresis carries the pad too: the camera one chunk on keeps everything within evict + pad.
        rig.settle(centreOf(1, 0, 0));
        check(rig.st.isResident(ChunkCoord{0, 0, 0}), "...and the chunk behind the camera is kept");
        // Far away, all of it goes.
        rig.settle(centreOf(40, 0, 0));
        check(rig.st.residentCount() == 0, "...and everything is evicted when the camera is far away");
        rig.clear();
    }
}

static void wideChecks() {
    AVER_INFO("-- wide");
    MapSource src;
    src.add(0, 0, 0);
    src.add(20, 0, 0);
    WideItem bridge;
    bridge.owner = ChunkCoord{20, 0, 0};
    bridge.minCm[0] = 4800;  bridge.minCm[1] = -100; bridge.minCm[2] = -100;
    bridge.maxCm[0] = 33800; bridge.maxCm[1] = 100;  bridge.maxCm[2] = 100;
    const ChunkCoord owner = bridge.owner;

    {   // CONTROL: no wide list, the owner is 20 chunks away and never loads
        Rig rig;
        StreamSettings s = plain(3, 5);
        s.verticalRadius = kAllLayers;
        s.padChunks = 2;
        rig.st.setSettings(s);
        rig.st.setSource(&src);
        rig.settle(Vec3{800, 0, 0});
        check(rig.st.isResident(ChunkCoord{0, 0, 0}) && !rig.st.isResident(owner), "CONTROL: without the wide list the far owner chunk is not loaded");
        rig.clear();
    }
    Rig rig;
    StreamSettings s = plain(3, 5);
    s.verticalRadius = kAllLayers;
    s.padChunks = 2;
    rig.st.setSettings(s);
    rig.st.setSource(&src);
    rig.st.setWide({bridge});
    // The camera at x = 800 is 4000 cm from the box's west edge: inside the load distance (3 chunks = 4800 cm).
    rig.settle(Vec3{800, 0, 0});
    check(rig.st.isResident(owner), "the owner chunk of a bridge whose box is 4000 cm away loads, 20 chunks from the camera");
    // 7800 cm from the box: past the load distance, inside the keep distance (5 chunks = 8000 cm).
    rig.settle(Vec3{-3000, 0, 0});
    check(rig.st.isResident(owner), "...is kept at 7800 cm (hysteresis)");
    rig.settle(Vec3{-4000, 0, 0});
    check(!rig.st.isResident(owner), "...and evicted at 8800 cm");
    rig.settle(Vec3{-3000, 0, 0});
    check(!rig.st.isResident(owner), "...and not reloaded at 7800 cm (the load distance is shorter than the keep distance)");
    rig.settle(Vec3{800, 0, 0});
    check(rig.st.isResident(owner), "...but reloaded when the box is near again");
    rig.clear();
}

// ---- PREPARE GATE ---------------------------------------------------------------------------------------------------------------

static void prepareChecks() {
    AVER_INFO("-- prepare gate");
    MapSource src;
    src.add(0, 0, 0); src.add(1, 0, 0); src.add(2, 0, 0); src.add(0, 1, 0);
    TestHost host;
    host.pendingFor[ChunkCoord{1, 0, 0}] = 3;
    host.failFor.insert(ChunkCoord{2, 0, 0});
    Rig rig;
    StreamSettings s = plain(2, 4);
    s.verticalRadius = kAllLayers;
    rig.st.setSettings(s);
    rig.st.setSource(&src);
    rig.st.setHost(&host);

    const Vec3 cam = centreOf(0, 0, 0);
    StreamStats a = rig.step(cam);
    check(rig.st.isResident(ChunkCoord{0, 0, 0}) && rig.st.isResident(ChunkCoord{0, 1, 0}), "chunks the host accepts load at once");
    check(!rig.st.isResident(ChunkCoord{1, 0, 0}) && a.preparePending == 1 && a.pendingLoads >= 1, "a chunk the host answers Pending for is held back and counted");
    check(rig.st.isResident(ChunkCoord{2, 0, 0}) && a.failedLoads == 1 && rig.st.entitiesOf(ChunkCoord{2, 0, 0}) &&
              rig.st.entitiesOf(ChunkCoord{2, 0, 0})->empty(),
          "a chunk the host refuses is resident and empty, and counted as failed");
    check(rig.st.pendingPrepareCount() == 1, "its decoded payload is held");

    for (int i = 0; i < 2; ++i) {
        a = rig.step(cam);   // the camera has not moved: the retry must not be skipped by the idle early-out
        check(!rig.st.isResident(ChunkCoord{1, 0, 0}) && a.preparePending == 1, "...and retried on every update with the camera standing still");
    }
    a = rig.step(cam);
    check(rig.st.isResident(ChunkCoord{1, 0, 0}) && a.loadedThisUpdate == 1 && a.preparePending == 0, "it loads on the update the host says Ready");
    check(src.loads[ChunkCoord{1, 0, 0}] == 1, "the payload was decoded exactly once, however long it waited");
    check(host.prepareCalls[ChunkCoord{1, 0, 0}] == 4 && host.restoreCalls[ChunkCoord{1, 0, 0}] == 1, "prepare was asked four times and restore once");
    for (int i = 0; i < 3; ++i) rig.step(cam);
    check(host.prepareCalls[ChunkCoord{2, 0, 0}] == 1, "a refused chunk is not asked again");
    check(rig.st.pendingPrepareCount() == 0, "nothing is held once everything resolved");
    rig.clear();

    // A held payload is dropped when its chunk stops being wanted.
    {
        TestHost stuck;
        stuck.pendingFor[ChunkCoord{1, 0, 0}] = 100000;
        Rig r2;
        r2.st.setSettings(s);
        r2.st.setSource(&src);
        r2.st.setHost(&stuck);
        r2.step(cam);
        check(r2.st.pendingPrepareCount() == 1, "a chunk stuck on Pending is held");
        r2.step(centreOf(60, 0, 0));
        check(r2.st.pendingPrepareCount() == 0, "...and its payload is let go when the camera leaves");
        r2.clear();
    }
}

// ---- BUDGETS --------------------------------------------------------------------------------------------------------------------

static void budgetChecks() {
    AVER_INFO("-- budgets");
    MapSource src;
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 5; ++y) src.add(x, y, 0);   // 40 chunks
    const Vec3 cam = centreOf(3, 2, 0);

    {   // CONTROL: no budget, one update
        TestHost host;
        Rig rig;
        StreamSettings s = plain(10, 12);
        s.verticalRadius = kAllLayers;
        rig.st.setSettings(s);
        rig.st.setSource(&src);
        rig.st.setHost(&host);
        const StreamStats a = rig.step(cam);
        check(a.loadedThisUpdate == 40, "CONTROL: with no budget all 40 chunks load in one update");
        rig.clear();
    }
    {   // a millisecond budget
        TestHost host;
        host.restoreSpinMs = 3.0;
        Rig rig;
        StreamSettings s = plain(10, 12);
        s.verticalRadius = kAllLayers;
        s.loadBudgetMs = 5.0;
        rig.st.setSettings(s);
        rig.st.setSource(&src);
        rig.st.setHost(&host);
        u32 minPer = 1000, maxPer = 0, total = 0;
        int updates = 0;
        for (int i = 0; i < 200; ++i) {
            const StreamStats a = rig.step(cam);
            ++updates;
            if (a.loadedThisUpdate) { minPer = std::min(minPer, a.loadedThisUpdate); maxPer = std::max(maxPer, a.loadedThisUpdate); }
            total += a.loadedThisUpdate;
            if (a.pendingLoads == 0) break;
        }
        // Each restore takes at least 3 ms: after one the 5 ms budget may remain, after two it cannot.
        check(minPer >= 1 && maxPer <= 2, "a 5 ms budget with 3 ms chunks loads one or two per update");
        check(total == 40 && updates >= 20 && updates <= 41, "...and still loads all 40 (the budget slows the load, it never stops it)");
        rig.clear();
    }
    {   // an entity cap
        TestHost host;
        Rig rig;
        StreamSettings s = plain(10, 12);
        s.verticalRadius = kAllLayers;
        s.loadEntityCap = 10;   // 4 entities a chunk: chunk 3 crosses it
        rig.st.setSettings(s);
        rig.st.setSource(&src);
        rig.st.setHost(&host);
        const StreamStats a = rig.step(cam);
        check(a.loadedThisUpdate == 3 && a.entitiesIn == 12, "an entity cap of 10 stops at the chunk that crosses it (3 chunks, 12 entities)");
        const StreamStats b = rig.settle(cam);
        check(b.pendingLoads == 0 && rig.st.residentCount() == 40, "...and the rest follows");
        rig.clear();
    }
    {   // an evict budget
        TestHost host;
        host.evictSpinMs = 3.0;
        Rig rig;
        StreamSettings s = plain(10, 12);
        s.verticalRadius = kAllLayers;
        s.evictBudgetMs = 4.0;
        rig.st.setSettings(s);
        rig.st.setSource(&src);
        rig.st.setHost(&host);
        rig.step(cam);
        check(rig.st.residentCount() == 40, "(40 resident)");
        u32 minPer = 1000, maxPer = 0, total = 0;
        for (int i = 0; i < 100; ++i) {
            const StreamStats a = rig.step(centreOf(500, 500, 0));
            if (a.evictedThisUpdate) { minPer = std::min(minPer, a.evictedThisUpdate); maxPer = std::max(maxPer, a.evictedThisUpdate); }
            total += a.evictedThisUpdate;
            if (rig.st.residentCount() == 0) break;
        }
        check(minPer >= 1 && maxPer <= 2 && total == 40 && rig.st.residentCount() == 0,
              "a 4 ms evict budget with 3 ms evictions takes one or two per update and finishes");
        rig.clear();
    }
}

// ---- PIN ------------------------------------------------------------------------------------------------------------------------

static void pinChecks() {
    AVER_INFO("-- pin");
    MapSource src;
    src.add(0, 0, 0);
    src.add(1, 0, 0);
    src.add(50, 0, 0);
    Rig rig;
    StreamSettings s = plain(3, 5);
    s.verticalRadius = kAllLayers;
    s.loadBudget = 1;
    rig.st.setSettings(s);
    rig.st.setSource(&src);
    rig.st.pin(ChunkCoord{50, 0, 0});
    rig.st.pin(ChunkCoord{99, 99, 99});   // the source holds nothing here
    rig.step(centreOf(0, 0, 0));
    check(rig.st.isResident(ChunkCoord{50, 0, 0}) && !rig.st.isResident(ChunkCoord{0, 0, 0}),
          "a pinned chunk 50 chunks away loads FIRST under a budget of one, ahead of the camera's own chunk");
    rig.settle(centreOf(0, 0, 0));
    check(rig.st.isResident(ChunkCoord{99, 99, 99}) && rig.st.entitiesOf(ChunkCoord{99, 99, 99}) && rig.st.entitiesOf(ChunkCoord{99, 99, 99})->empty(),
          "a pinned chunk the source has nothing for is resident and empty");
    check(rig.st.pinned(ChunkCoord{50, 0, 0}), "pinned() answers");
    rig.settle(centreOf(-30, 0, 0));
    check(rig.st.isResident(ChunkCoord{50, 0, 0}) && !rig.st.isResident(ChunkCoord{0, 0, 0}), "a pin survives the camera leaving; the unpinned chunks go");
    rig.st.pin(ChunkCoord{50, 0, 0}, false);
    rig.step(centreOf(-30, 0, 0));
    check(!rig.st.isResident(ChunkCoord{50, 0, 0}), "unpinning lets the distance rule evict it on the next update");
    rig.clear();
}

// ---- SCATTER HOST ---------------------------------------------------------------------------------------------------------------

static void scatterChecks() {
    AVER_INFO("-- scatter host");
    MapSource src;
    for (int x = -5; x < 400; ++x)
        for (int y = -3; y <= 3; ++y) src.add(x, y, 0);
    ScatterChunkHost scatter;
    Rig a, b;
    StreamSettings s;
    s.loadRadius = 3; s.evictRadius = 5; s.loadBudget = 2; s.evictBudget = 4; s.verticalRadius = 1; s.leadSeconds = 1.5f;
    a.st.setSettings(s); a.st.setSource(&src);
    b.st.setSettings(s); b.st.setSource(&src); b.st.setHost(&scatter);
    bool same = true;
    Vec3 pos{0, 0, 800};
    for (int i = 0; i < 160; ++i) {
        std::vector<StreamSource> sa{StreamSource{pos, Vec3{30000, 0, 0}}};
        a.st.setSources(sa); b.st.setSources(sa);
        const StreamStats x = a.st.update(a.w, a.bodies, &a.freed);
        const StreamStats y = b.st.update(b.w, b.bodies, &b.freed);
        a.w.flush();
        same = same && x.loadedThisUpdate == y.loadedThisUpdate && x.evictedThisUpdate == y.evictedThisUpdate && x.entitiesIn == y.entitiesIn &&
               x.entitiesOut == y.entitiesOut && x.pendingLoads == y.pendingLoads && x.residentChunks == y.residentChunks &&
               x.residentEntities == y.residentEntities && x.failedLoads == y.failedLoads;
        pos.x += 600.0f;
    }
    auto sorted = [](const ChunkStreamer& st) {
        std::vector<ChunkCoord> v = st.residentChunks();
        std::sort(v.begin(), v.end(), [](const ChunkCoord& p, const ChunkCoord& q) { return std::tie(p.x, p.y, p.z) < std::tie(q.x, q.y, q.z); });
        return v;
    };
    check(same, "a 160-step flight gives identical stats with no host and with ScatterChunkHost");
    check(sorted(a.st) == sorted(b.st) && a.st.residentCount() > 0, "...and the same resident set at the end");
    a.clear(); b.clear();
}

// ---- OCCUPANCY ------------------------------------------------------------------------------------------------------------------

static u32 rng(u32& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

static void occupancyChecks(const std::filesystem::path& root) {
    AVER_INFO("-- occupancy");
    namespace fs = std::filesystem;
    // Content across the seam between region 0 and region 1 in X (global chunk 511 | 512 is at 817,600 | 819,200 cm).
    fmt::OcWorldData level;
    level.name = "Seam";
    u32 seed = 12345;
    const f64 baseX = 818400.0;
    for (int i = 0; i < 400; ++i) {
        const f64 x = baseX + static_cast<f64>(static_cast<i32>(rng(seed) % 40) - 20) * S + 400.0;
        const f64 y = static_cast<f64>(static_cast<i32>(rng(seed) % 30) - 15) * S + 400.0;
        const f64 z = static_cast<f64>(static_cast<i32>(rng(seed) % 8) - 3) * S + 400.0;
        level.placements.push_back(place("Meshes/Box.ocmesh", x, y, z, 0x5000 + static_cast<u64>(i)));
    }
    const fs::path packedDir = root / "seam-packed";
    CookReport rep;
    checkWhy(cookTo(level, packedDir, &rep), "the seam level cooks");
    check(rep.regions == 2, "...into two regions");

    // The same chunks as version 2 (unpacked) region files with their own index.
    const fs::path v2Dir = root / "seam-v2";
    {
        std::error_code ec;
        fs::create_directories(v2Dir, ec);
        LevelAssetLibrary lib;
        CookPlan plan;
        CookReport pr;
        CookInput in = cookInput(&lib, "");
        planAuthoredCook(level, in, CookOptions{}, plan, pr, &why);
        RegionIndex ix;
        ix.kind = kIndexKindCooked;
        ix.chunkSizeCm = S;
        for (const CookPlan::Region& reg : plan.regions) {
            std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks;
            for (usize ci = reg.first; ci < reg.end; ++ci) {
                ChunkPayload p = plan.payload(ci);
                chunks.emplace_back(localOf(p.coord), std::move(p));
            }
            RegionWriteDesc d;
            d.coord = reg.coord;
            d.chunkSizeCm = S;
            d.packed = false;
            const std::string name = regionFileName(reg.coord);
            writeRegion((v2Dir / name).string(), d, std::move(chunks), &why);
            RegionFile f;
            f.open((v2Dir / name).string(), nullptr);
            RegionEntry e;
            e.coord = reg.coord;
            e.contentHash = f.header().contentHash;
            e.chunkCount = f.header().chunkCount;
            e.relativePath = name;
            ix.add(e);
        }
        writeIndex((v2Dir / kCookIndexName).string(), ix, nullptr);
    }

    for (int layout = 0; layout < 2; ++layout) {
        RegionChunkSource src;
        const fs::path dir = layout == 0 ? packedDir : v2Dir;
        checkWhy(src.open((dir / kCookIndexName).string(), dir.string(), &why), layout == 0 ? "the packed set opens" : "the version 2 set opens");
        u32 s2 = 777u + static_cast<u32>(layout);
        int mismatches = 0, boxes = 0, nonEmpty = 0;
        for (int t = 0; t < 120; ++t) {
            ChunkBox box;
            box.min.x = 511 - 24 + static_cast<i32>(rng(s2) % 24); box.max.x = box.min.x + static_cast<i32>(rng(s2) % 30);
            box.min.y = -16 + static_cast<i32>(rng(s2) % 20);       box.max.y = box.min.y + static_cast<i32>(rng(s2) % 25);
            const bool unbounded = (t % 3) == 0;
            if (unbounded) { box.min.z = kChunkAxisMin; box.max.z = kChunkAxisMax; }
            else { box.min.z = -4 + static_cast<i32>(rng(s2) % 6); box.max.z = box.min.z + static_cast<i32>(rng(s2) % 6); }
            std::set<std::tuple<i32, i32, i32>> got, want;
            src.forEachOccupied(box, [&](const ChunkCoord& c) { got.insert({c.x, c.y, c.z}); });
            const i32 zlo = unbounded ? -8 : box.min.z, zhi = unbounded ? 8 : box.max.z;   // the content lies in layers -3..4
            for (i32 z = zlo; z <= zhi; ++z)
                for (i32 y = box.min.y; y <= box.max.y; ++y)
                    for (i32 x = box.min.x; x <= box.max.x; ++x)
                        if (src.has(ChunkCoord{x, y, z})) want.insert({x, y, z});
            ++boxes;
            if (!want.empty()) ++nonEmpty;
            if (got != want) ++mismatches;
        }
        check(mismatches == 0 && nonEmpty > 40, std::string(layout == 0 ? "packed" : "version 2") + ": forEachOccupied equals a has() scan on 120 boxes across the region seam, bounded and unbounded in Z");
        check(src.occupancyTables() == 2, "...from one occupancy table per region");
    }
    // A box nowhere near anything.
    {
        RegionChunkSource src;
        src.open((packedDir / kCookIndexName).string(), packedDir.string(), nullptr);
        u32 hits = 0;
        ChunkBox farBox;
        farBox.min = ChunkCoord{100000, 100000, kChunkAxisMin};
        farBox.max = ChunkCoord{100010, 100010, kChunkAxisMax};
        src.forEachOccupied(farBox, [&](const ChunkCoord&) { ++hits; });
        check(hits == 0, "a box over no region finds nothing");
    }
}

// ---- CHUNKED EQUALS FLAT --------------------------------------------------------------------------------------------------------

static fmt::OcWorldData gridLevel(f64 baseX, f64 baseY) {
    fmt::OcWorldData level;
    level.name = "Grid";
    for (int i = 0; i < 360; ++i) {
        // On a 0.25 cm grid, within +-12,000 cm of the base, so every number is exact in f32 whichever way it is formed.
        const f64 x = baseX + static_cast<f64>((i * 7919) % 96000 - 48000) * 0.25;
        const f64 y = baseY + static_cast<f64>((i * 104729) % 96000 - 48000) * 0.25;
        const f64 z = static_cast<f64>((i * 13) % 800) * 0.25;
        fmt::OcWorldPlacement p = place("Meshes/P" + std::to_string(i) + ".ocmesh", x, y, z, 0x7000 + static_cast<u64>(i));
        if (i % 3 == 0) { p.yaw = static_cast<f64>(i % 360); p.pitch = 15.0; }
        if (i % 5 == 0) { p.sx = 2.0; p.sy = 0.5; p.sz = 1.5; }
        if (i % 11 == 0) p.visible = false;
        level.placements.push_back(p);
        if (i % 7 == 0) {   // a child, parent-relative
            fmt::OcWorldPlacement c = place("Meshes/P" + std::to_string(1000 + i) + ".ocmesh", 40.25, -12.5, 8.0, 0x9000 + static_cast<u64>(i));
            c.parent = static_cast<i32>(level.placements.size()) - 1;
            c.yaw = 45.0;
            level.placements.push_back(c);
        }
    }
    return level;
}

static void flatChecks(const std::filesystem::path& root) {
    AVER_INFO("-- chunked equals flat");
    namespace fs = std::filesystem;
    scene::World& w = scene::World::instance();
    const i64 step = kOriginStepCm;
    const OriginCm origins[] = {OriginCm{0, 0}, OriginCm{61 * step, -61 * step}};
    for (int k = 0; k < 2; ++k) {
        const OriginCm O = origins[k];
        const fmt::OcWorldData level = gridLevel(static_cast<f64>(O.x), static_cast<f64>(O.y));
        const fs::path dir = root / ("flat" + std::to_string(k));
        CookReport rep;
        checkWhy(cookTo(level, dir, &rep), k == 0 ? "the grid level cooks (origin zero)" : "the grid level cooks 6,000 km out");
        check(rep.levelGlobal == 0 && rep.chunkOwned == level.placements.size(), "everything is chunk-owned");

        // FLAT: the placements instantiated the way both hosts do, against the same origin.
        InstantiateOptions iopt;
        iopt.createBodies = false;
        iopt.origin = O;
        const LevelInstance flat = instantiate(level, iopt);
        std::unordered_map<std::string, scene::Entity> flatByName;
        for (const scene::Entity e : flat.entities) flatByName.emplace(w.name(e), e);
        check(flatByName.size() == level.placements.size(), "(flat: one entity per placement, all names distinct)");

        // CHUNKED: the cooked set streamed with the camera over the middle of the content.
        StreamSettings ss = authoredStreamSettings(S, 60000.0f, 80000.0f);
        ss.loadBudgetMs = ss.loadBudgetFarMs = ss.evictBudgetMs = 0.0;
        ss.loadEntityCap = 0;
        ss.leadSeconds = 0.0f;
        ChunkWorld cw;
        checkWhy(cw.openAuthored(dir.string(), ss, nullptr, &why), "the set opens");
        std::vector<i32> freed;
        StreamStats s;
        for (int i = 0; i < 20; ++i) { s = cw.update(w, Vec3{0, 0, 0}, Vec3{0, 0, 0}, 0.016f, &freed, O); if (s.pendingLoads == 0) break; }
        check(cw.streamedEntities().size() == level.placements.size(), "the streamed entity count equals the placement count");

        usize matched = 0, positionDiffs = 0, rotationDiffs = 0, scaleDiffs = 0, parentDiffs = 0, unmatched = 0;
        for (const scene::Entity e : cw.streamedEntities()) {
            const auto it = flatByName.find(w.name(e));
            if (it == flatByName.end()) { ++unmatched; continue; }
            ++matched;
            const Transform& a = w.localTransform(e);
            const Transform& f = w.localTransform(it->second);
            if (a.position.x != f.position.x || a.position.y != f.position.y || a.position.z != f.position.z) ++positionDiffs;
            if (a.rotation.x != f.rotation.x || a.rotation.y != f.rotation.y || a.rotation.z != f.rotation.z || a.rotation.w != f.rotation.w) ++rotationDiffs;
            if (a.scale.x != f.scale.x || a.scale.y != f.scale.y || a.scale.z != f.scale.z) ++scaleDiffs;
            const bool aRoot = w.parent(e) == scene::kInvalidEntity, fRoot = w.parent(it->second) == scene::kInvalidEntity;
            if (aRoot != fRoot) ++parentDiffs;
            else if (!aRoot && std::string(w.name(w.parent(e))) != std::string(w.name(w.parent(it->second)))) ++parentDiffs;
        }
        check(matched == level.placements.size() && unmatched == 0, "every streamed entity has its flat twin by name");
        check(positionDiffs == 0, "ChunkedEqualsFlat: positions are bit-identical");
        check(rotationDiffs == 0 && scaleDiffs == 0, "...rotations and scales too");
        check(parentDiffs == 0, "...and so is the hierarchy");

        // The mesh renderer matches too: mesh id, visibility.
        usize rendererDiffs = 0;
        for (const scene::Entity e : cw.streamedEntities()) {
            const auto it = flatByName.find(w.name(e));
            if (it == flatByName.end()) continue;
            const auto* ma = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            const auto* mf = w.component<scene::CMeshRenderer>(it->second, scene::kComponentMeshRenderer);
            if (!ma || !mf || ma->mesh != mf->mesh || (ma->flags & scene::kMeshRendererVisible) != (mf->flags & scene::kMeshRendererVisible)) ++rendererDiffs;
        }
        check(rendererDiffs == 0, "...and the mesh renderer (mesh id, visibility)");

        cw.shutdown(w, freed);
        for (const scene::Entity e : flat.entities) w.destroy(e);
        w.flush();
    }
}

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aver-authoredstream-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    columnChecks(root);
    padChecks();
    wideChecks();
    prepareChecks();
    budgetChecks();
    pinChecks();
    scatterChecks();
    occupancyChecks(root);
    flatChecks(root);

    fs::remove_all(root, ec);
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
