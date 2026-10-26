// Aver.World: the chunk path with a floating origin (LARGE_WORLD 2.4.2, tests 5.1 ChunkOriginTest). Exit code = failure count.
//
// The streamer, the partitioner and the restore pass know the origin now. What this pins:
//   1  restore places a root at (chunk origin - O) taken in i64 before the one f32 narrowing, for O in {0, +1 step, -2 steps};
//      with O = 0 it is the old formula bit for bit
//   2  ownerChunkOf / partitionWorld / capture / captureAll split origin-relative f32 + O in f64; O = 0 is the old split; a
//      capture at O restored at O is the identity
//   3  ZERO CHURN: shift the origin between two updates (scene roots moved by a fake participant) and nothing loads, nothing
//      evicts, the resident set and every entity's absolute position are unchanged -- with a negative control that forgets the origin
//   4  a source past StreamSettings::sourceLimitCm is not streamed (and is counted); inside it, it is
//   5  the generator's lattice is formed from an f64 absolute position: pcg::sampleInfiniteAbs agrees with sampleInfinite where
//      f32 is exact and separates cells where f32 cannot; a chunk 100 km out generates the same bytes every time; the f64
//      height seam is used in preference to the f32 one
//   6  an index made by another generator version / seed is named on open, a matching one is not
//   7  the body a host's createBody returns is attached to its entity and handed back when the chunk evicts
// No GPU, no window, no .NET.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/pcg/PcgVolume.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/ChunkPartition.hpp"
#include "aver/world/ChunkPayload.hpp"
#include "aver/world/ChunkSource.hpp"
#include "aver/world/ChunkStreamer.hpp"
#include "aver/world/ChunkWorld.hpp"
#include "aver/world/RegionIndex.hpp"
#include "aver/world/WorldOrigin.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static int g_verWarn = 0, g_seedWarn = 0;
static void captureWarns(void*, LogLevel level, std::string_view m) {
    if (level != LogLevel::Warn) return;
    if (m.find("generator version") != std::string_view::npos) ++g_verWarn;
    if (m.find("world seed") != std::string_view::npos) ++g_seedWarn;
}

static i64 fdiv(i64 a, i64 b) {
    const i64 q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// Every z == 0 chunk in an x range and |y| <= 2 holds one entity at chunk-local (400, 800, 50), optionally with a body.
class GridSource final : public IChunkSource {
public:
    i32 xLo = -4096, xHi = 4096;
    bool withBody = false;
    bool has(const ChunkCoord& c) override { return c.z == 0 && c.x >= xLo && c.x <= xHi && c.y >= -2 && c.y <= 2; }
    bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) override {
        if (!has(c)) { if (why) *why = "no such chunk"; return false; }
        out = ChunkPayload{};
        out.coord = c;
        out.chunkSizeCm = kDefaultChunkSizeCm;
        PayloadEntity e;
        e.name = "g_" + std::to_string(c.x) + "_" + std::to_string(c.y);
        e.objectId = (static_cast<u64>(static_cast<u32>(c.x)) << 32) | static_cast<u32>(c.y);
        e.local.position = Vec3{400.0f, 800.0f, 50.0f};
        e.hasBody = withBody;
        e.bodyHalfExtentCm[0] = e.bodyHalfExtentCm[1] = e.bodyHalfExtentCm[2] = 50.0f;
        out.entities.push_back(std::move(e));
        return true;
    }
};

static std::vector<ChunkCoord> sortedChunks(const ChunkStreamer& st) {
    std::vector<ChunkCoord> v = st.residentChunks();
    std::sort(v.begin(), v.end(), [](const ChunkCoord& a, const ChunkCoord& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    return v;
}

// name -> absolute position (f64 cm) of every entity the streamer holds, read through the origin it was told.
static std::map<std::string, Vec3d> absolutes(const ChunkStreamer& st, scene::World& w, OriginCm o) {
    std::map<std::string, Vec3d> out;
    for (const ChunkCoord& c : st.residentChunks()) {
        const std::vector<scene::Entity>* es = st.entitiesOf(c);
        if (!es) continue;
        for (const scene::Entity e : *es) out[w.name(e)] = absOf(worldPosOfLocal(w.localTransform(e).position, o));
    }
    return out;
}

static bool sameAbs(const std::map<std::string, Vec3d>& a, const std::map<std::string, Vec3d>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        if (it == b.end()) return false;
        if (kv.second.x != it->second.x || kv.second.y != it->second.y || kv.second.z != it->second.z) return false;
    }
    return true;
}

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) w.destroy(e);
    w.flush();
}

int main() {
    const i32 S = kDefaultChunkSizeCm;
    const i64 STEP = kOriginStepCm;
    scene::World& w = scene::World::instance();

    // ---- (1) restore against an origin -------------------------------------------------------------------------
    {
        ChunkPayload p;
        p.coord = ChunkCoord{1034, -7, 0};
        p.chunkSizeCm = S;
        PayloadEntity root;
        root.name = "root";
        root.local.position = Vec3{123.456f, 1599.0f, 20.0f};
        PayloadEntity kid;
        kid.name = "kid";
        kid.parent = 0;
        kid.local.position = Vec3{5.0f, 6.0f, 7.0f};
        p.entities = {root, kid};

        for (const i64 ox : {i64(0), STEP, -2 * STEP}) {
            clearWorld(w);
            // At origin zero the chunk is 1.65 km out (a large f32, the old formula); at any other origin it sits 16 km from it.
            ChunkPayload q = p;
            q.coord.x = ox == 0 ? 1034 : static_cast<i32>(ox / 1600 + 10);
            RestoreOptions ro;
            ro.origin = OriginCm{ox, 0};
            const std::vector<scene::Entity> made = restore(q, w, ro);
            check(made.size() == 2 && made[0] != scene::kInvalidEntity && made[1] != scene::kInvalidEntity,
                  "restore made both entities at origin " + std::to_string(ox));
            if (made.size() != 2) continue;
            const Vec3 pos = w.localTransform(made[0]).position;
            // The absolute position the entity names must be the chunk origin plus the chunk-local offset, within the f32 grid at |pos|.
            const f64 wantX = static_cast<f64>(chunkOriginCmAxis(q.coord.x, S)) + static_cast<f64>(123.456f);
            const f64 wantY = static_cast<f64>(chunkOriginCmAxis(q.coord.y, S)) + 1599.0;
            const Vec3d got = absOf(worldPosOfLocal(pos, OriginCm{ox, 0}));
            check(std::fabs(got.x - wantX) <= 0.5 * f32GridAtCm(pos.x) + 1e-3, "origin " + std::to_string(ox) + ": x names the right absolute position");
            check(std::fabs(got.y - wantY) <= 0.5 * f32GridAtCm(pos.y) + 1e-3, "origin " + std::to_string(ox) + ": y names the right absolute position");
            const Vec3 kp = w.localTransform(made[1]).position;
            check(kp.x == 5.0f && kp.y == 6.0f && kp.z == 7.0f, "origin " + std::to_string(ox) + ": a child stays parent-relative");
            if (ox == 0) {
                const f32 oldX = 123.456f + static_cast<f32>(chunkOriginCmAxis(q.coord.x, S));
                const f32 oldY = 1599.0f + static_cast<f32>(chunkOriginCmAxis(q.coord.y, S));
                check(pos.x == oldX && pos.y == oldY && pos.z == 20.0f, "origin zero is the old formula, bit for bit");
            } else {
                check(std::fabs(pos.x) < 20000.0f, "a restored root lands near the origin it was told, not 100 km out");
            }
        }
        clearWorld(w);
    }

    // ---- (2) owner chunk, partition, capture ------------------------------------------------------------------
    {
        struct R { const char* name; Vec3 p; };
        const R roots[] = {{"a", Vec3{100, 100, 0}}, {"b", Vec3{-1, -1, 0}}, {"c", Vec3{1599, 0, 0}},
                           {"d", Vec3{-1600, 3200, 5}}, {"e", Vec3{1600, 0, 0}}};
        for (const i64 ox : {i64(0), STEP, -2 * STEP}) {
            clearWorld(w);
            std::vector<scene::Entity> es;
            for (const R& r : roots) {
                Transform xf;
                xf.position = r.p;
                es.push_back(w.create(r.name, scene::kInvalidEntity, xf));
            }
            const OriginCm O{ox, 0};
            bool ownersOk = true;
            std::set<std::tuple<i32, i32, i32>> want;
            for (usize i = 0; i < es.size(); ++i) {
                const i64 ax = static_cast<i64>(roots[i].p.x) + ox;
                const ChunkCoord ref{static_cast<i32>(fdiv(ax, S)), static_cast<i32>(fdiv(static_cast<i64>(roots[i].p.y), S)),
                                     static_cast<i32>(fdiv(static_cast<i64>(roots[i].p.z), S))};
                ownersOk = ownersOk && ownerChunkOf(w, es[i], S, O) == ref;
                want.insert({ref.x, ref.y, ref.z});
                if (ox == 0) ownersOk = ownersOk && ownerChunkOf(w, es[i], S) == splitCm(roots[i].p, S).chunk;
            }
            check(ownersOk, "ownerChunkOf splits position + origin (origin " + std::to_string(ox) + ")");

            PartitionOptions po;
            po.chunkSizeCm = S;
            const Partition part = partitionWorld(w, po, O);
            std::set<std::tuple<i32, i32, i32>> got;
            for (const auto& kv : part.chunks) got.insert({kv.first.x, kv.first.y, kv.first.z});
            check(got == want && part.rootCount == 5 && part.entityCount == 5, "partitionWorld keys the same chunks (origin " + std::to_string(ox) + ")");
            if (ox == 0) {
                const Partition legacy = partitionWorld(w);
                std::set<std::tuple<i32, i32, i32>> old;
                for (const auto& kv : legacy.chunks) old.insert({kv.first.x, kv.first.y, kv.first.z});
                check(old == got, "partitionWorld with no origin argument is the zero-origin result");
            }

            // capture at O, destroy, restore at O: every root comes back where it was, bit for bit.
            const auto payloads = captureAll(w, part, S, O);
            std::map<std::string, Vec3> before;
            for (usize i = 0; i < es.size(); ++i) before[roots[i].name] = roots[i].p;
            bool localsOk = true;
            for (const auto& kv : payloads)
                for (const PayloadEntity& pe : kv.second.entities)
                    localsOk = localsOk && pe.local.position.x >= 0.0f && pe.local.position.x < 1600.0f &&
                               pe.local.position.y >= 0.0f && pe.local.position.y < 1600.0f;
            check(localsOk, "captured root positions are chunk-local on [0, 1600) at origin " + std::to_string(ox));
            for (const scene::Entity e : es) w.destroy(e);
            w.flush();
            RestoreOptions ro;
            ro.origin = O;
            for (const auto& kv : payloads) restore(kv.second, w, ro);
            bool back = w.count() == 5;
            for (const auto& kv : before) {
                const scene::Entity e = w.find(kv.first);
                if (e == scene::kInvalidEntity) { back = false; continue; }
                const Vec3 p = w.localTransform(e).position;
                back = back && p.x == kv.second.x && p.y == kv.second.y && p.z == kv.second.z;
            }
            check(back, "capture at O then restore at O is the identity (origin " + std::to_string(ox) + ")");
        }
        clearWorld(w);
    }

    // ---- (3) zero churn across a rebase -----------------------------------------------------------------------
    {
        clearWorld(w);
        GridSource src;
        src.xLo = 1000; src.xHi = 1100;
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.loadRadius = 2; s.evictRadius = 4; s.verticalRadius = 0;
        s.loadBudget = 0; s.evictBudget = 0;   // unbounded: one update settles
        s.leadSeconds = 0.0f;
        st.setSettings(s);
        st.setSource(&src);

        const OriginCm O0{0, 0}, O1{STEP, 0};
        const f32 camAbsX = 1'700'000.0f;
        std::vector<i32> freed;
        st.setSources({Vec3{camAbsX, 0.0f, 50.0f}});
        const StreamStats a = st.update(w, bodies, &freed, O0);
        w.flush();
        const std::vector<ChunkCoord> chunksBefore = sortedChunks(st);
        const auto absBefore = absolutes(st, w, O0);
        check(a.loadedThisUpdate == 25 && a.residentChunks == 25 && a.residentEntities == 25, "the first update loads the 5 x 5 window");
        check(st.isResidentAt(Vec3{camAbsX, 0.0f, 50.0f}), "the viewer's own chunk is resident at origin zero");

        // The fake participant: every scene root moves by -delta, then the host reports the new frame.
        const Vec3 dF{static_cast<f32>(STEP), 0.0f, 0.0f};
        for (const ChunkCoord& c : st.residentChunks())
            if (const auto* es = st.entitiesOf(c))
                for (const scene::Entity e : *es) {
                    const Vec3 p = w.localTransform(e).position;
                    w.setLocalPosition(e, Vec3{p.x - dF.x, p.y - dF.y, p.z - dF.z});
                }
        w.flush();
        st.setSources({Vec3{camAbsX - dF.x, 0.0f, 50.0f}});
        const StreamStats b = st.update(w, bodies, &freed, O1);
        w.flush();
        check(b.loadedThisUpdate == 0 && b.evictedThisUpdate == 0, "a rebase between two updates loads nothing and evicts nothing");
        check(b.entitiesIn == 0 && b.entitiesOut == 0, "...and creates and destroys no entity");
        check(b.residentChunks == a.residentChunks && b.residentEntities == a.residentEntities, "...the resident counts are identical");
        check(sortedChunks(st) == chunksBefore, "...and so is the resident set, coordinate by coordinate");
        check(sameAbs(absolutes(st, w, O1), absBefore), "...and every entity names the same absolute position through the new origin");
        check(st.origin() == O1 && st.isResidentAt(Vec3{camAbsX - dF.x, 0.0f, 50.0f}), "isResidentAt reads local positions against the origin of the last update");
        check(st.isResidentAt(worldPosOfAbs(static_cast<f64>(camAbsX), 0.0, 50.0)), "...and a WorldPos names its chunk directly");

        // The WorldPos form of a source names the same anchor chunk as the f32 form.
        {
            ChunkStreamer s2;
            s2.setSettings(s);
            s2.setSource(&src);
            std::vector<StreamSource> srcs = {StreamSource::at(worldPosOfAbs(static_cast<f64>(camAbsX), 0.0, 50.0), O0)};
            s2.setSources(srcs);
            BodyRegistry b2;
            std::vector<i32> f2;
            s2.update(w, b2, &f2, O0);
            w.flush();
            check(sortedChunks(s2) == chunksBefore, "a StreamSource built from a WorldPos streams the same window");
            std::vector<i32> f3;
            s2.unloadAll(w, b2, f3);
            w.flush();
        }

        // Control: a host that shifted the scene but forgot to tell the streamer sees the camera 1.6 km "out of the world".
        st.setSources({Vec3{camAbsX - dF.x, 0.0f, 50.0f}});
        const StreamStats c = st.update(w, bodies, &freed, O0);
        w.flush();
        check(c.evictedThisUpdate > 0, "control: the same sources against the OLD origin evict (the origin argument is what keeps the window)");
        std::vector<i32> drop;
        st.unloadAll(w, bodies, drop);
        w.flush();
        clearWorld(w);
    }

    // ---- (4) the one-f32-frame limit --------------------------------------------------------------------------
    {
        clearWorld(w);
        GridSource src;
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.loadRadius = 1; s.evictRadius = 3; s.verticalRadius = 0;
        s.loadBudget = 0; s.evictBudget = 0;
        s.leadSeconds = 0.0f;
        s.sourceLimitCm = kSubMmCliffCm;
        st.setSettings(s);
        st.setSource(&src);
        std::vector<i32> freed;

        st.setSources({Vec3{2'000'000.0f, 0.0f, 50.0f}});
        StreamStats farS = st.update(w, bodies, &freed, OriginCm{0, 0});
        check(farS.residentChunks == 0 && farS.skippedSources == 1, "a source 2 km past the cliff is not streamed, and is counted");
        farS = st.update(w, bodies, &freed, OriginCm{0, 0});
        check(farS.residentChunks == 0 && farS.skippedSources == 1, "...and is still counted on the next update (the warning is once, the count is not)");

        st.setSources({Vec3{1'000'000.0f, 0.0f, 50.0f}});
        const StreamStats nearS = st.update(w, bodies, &freed, OriginCm{0, 0});
        w.flush();
        check(nearS.residentChunks == 9 && nearS.skippedSources == 0, "a source inside the cliff streams its 3 x 3 window");

        // Two sources: only the one past the limit is dropped.
        st.setSources({StreamSource{Vec3{1'000'000.0f, 0.0f, 50.0f}}, StreamSource{Vec3{0.0f, -5'000'000.0f, 50.0f}}});
        const StreamStats mixed = st.update(w, bodies, &freed, OriginCm{0, 0});
        w.flush();
        check(mixed.skippedSources == 1 && mixed.residentChunks == 9, "of two sources only the one past the limit is skipped");

        // Limit off (the default) is the old behaviour: it streams wherever it is.
        ChunkStreamer unlimited;
        StreamSettings s0 = s;
        s0.sourceLimitCm = 0;
        unlimited.setSettings(s0);
        unlimited.setSource(&src);
        BodyRegistry b2;
        unlimited.setSources({Vec3{2'000'000.0f, 0.0f, 50.0f}});
        const StreamStats o = unlimited.update(w, b2, &freed, OriginCm{0, 0});
        w.flush();
        check(o.residentChunks == 9 && o.skippedSources == 0, "sourceLimitCm = 0 keeps the pre-origin behaviour: every source streams");
        std::vector<i32> drop;
        st.unloadAll(w, bodies, drop);
        unlimited.unloadAll(w, b2, drop);
        w.flush();
        clearWorld(w);
    }

    // ---- (5) the generator is a function of the f64 absolute position ----------------------------------------
    {
        pcg::InfiniteSpec spec{};
        spec.seed = 12345;
        spec.layerCount = 1;
        spec.layers[0].frequency = 1.0f;
        spec.layers[0].amplitude = 1.0f;
        spec.layers[0].octaves = 3;
        spec.layers[0].lacunarity = 2.0f;
        spec.layers[0].gain = 0.5f;
        spec.layers[0].seedOffset = 0;
        spec.cellSizeCm = 1600.0f;
        spec.coverageFloor = 0.0f;
        spec.coverageBias = 1.0f;

        // Where f32 is exact the two samplers are the same function.
        int mismatches = 0, n = 0;
        for (int i = 0; i < 400; ++i) {
            const f64 x = -3000.0 + 15.4173 * i + 0.37, y = 2900.0 - 14.8291 * i + 0.11, z = 3.0 * (i % 7);
            const f32 f = pcg::sampleInfinite(spec, static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z));
            const f32 d = pcg::sampleInfiniteAbs(spec, x, y, z);
            ++n;
            if (f != d) ++mismatches;
        }
        check(mismatches == 0, "sampleInfiniteAbs equals sampleInfinite near the origin (" + std::to_string(n) + " samples)");

        // At 10^8 cm an f32 ulp is 8 cm: two points 1 cm apart across a cell edge are one number. The f64 form keeps them apart.
        int f32Same = 0, absApart = 0;
        for (int seed = 1; seed <= 16; ++seed) {
            pcg::InfiniteSpec sp = spec;
            sp.seed = seed * 7919;
            sp.layers[0].octaves = 1;
            const f32 a32 = pcg::sampleInfinite(sp, static_cast<f32>(1.0e8 - 0.5), 100.0f, 0.0f);
            const f32 b32 = pcg::sampleInfinite(sp, static_cast<f32>(1.0e8 + 0.5), 100.0f, 0.0f);
            if (a32 == b32) ++f32Same;
            const f32 aa = pcg::sampleInfiniteAbs(sp, 1.0e8 - 0.5, 100.0, 0.0);
            const f32 bb = pcg::sampleInfiniteAbs(sp, 1.0e8 + 0.5, 100.0, 0.0);
            if (aa != bb) ++absApart;
        }
        check(f32Same == 16, "control: the f32 sampler cannot tell the two sides of a cell edge apart at 10^8 cm");
        check(absApart >= 14, "the f64 sampler sees the edge (cells hash apart for nearly every seed)");
        const f32 s1 = pcg::sampleInfiniteAbs(spec, 1.0e8 + 0.5, 100.0, 0.0);
        const f32 s2 = pcg::sampleInfiniteAbs(spec, 1.0e8 + 1.5, 100.0, 0.0);
        check(s1 == s2, "two points in the same cell 10^8 cm out sample the same value");

        // A chunk 100 km out: pure function of (settings, coord), non-empty, chunk-local.
        GeneratorSettings gs;
        gs.worldSeed = 0xA11CE5ull;
        gs.samplesPerAxis = 3;
        gs.threshold = 0.0f;
        check(gs.generatorVersion == 2, "the generator rules are version 2");
        GeneratedChunkSource g(gs);
        const ChunkCoord farChunk{6250, 3, 0};
        const ChunkPayload p1 = g.generate(farChunk);
        const ChunkPayload p2 = GeneratedChunkSource(gs).generate(farChunk);
        check(!p1.entities.empty() && encodeChunk(p1) == encodeChunk(p2), "a chunk 100 km out generates the same bytes from a fresh generator");
        bool inChunk = true;
        for (const PayloadEntity& e : p1.entities)
            inChunk = inChunk && e.local.position.x >= 0.0f && e.local.position.x < 1600.0f && e.local.position.y >= 0.0f && e.local.position.y < 1600.0f;
        check(inChunk, "...with chunk-local positions");

        // The f64 height seam is preferred, and is handed the f64 absolute position.
        {
            GeneratorSettings hs = gs;
            hs.heightSource = [](f32, f32, f32& z) { z = 777.0f; return true; };
            hs.heightSourceAbs = [](f64 wx, f64, f32& z) { z = static_cast<f32>(0.01 * wx); return true; };
            const ChunkPayload hp = GeneratedChunkSource(hs).generate(farChunk);
            const f64 ox = static_cast<f64>(chunkOriginCmAxis(farChunk.x, S));
            bool z = !hp.entities.empty();
            for (const PayloadEntity& e : hp.entities) {
                const f32 want = static_cast<f32>(static_cast<f64>(static_cast<f32>(0.01 * (ox + static_cast<f64>(e.local.position.x)))) - 0.0);
                z = z && e.local.position.z == want;
            }
            check(z, "heightSourceAbs wins over heightSource and sees f64 absolute x");
        }
    }

    // ---- (6) the index's generator version and seed are compared on open --------------------------------------
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-chunkorigin-test-index";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        RegionIndex ix;
        ix.chunkSizeCm = S;
        ix.generatorVersion = 1;
        ix.worldSeed = 99;
        RegionEntry re;
        re.coord = RegionCoord{0, 0, 0};
        re.contentHash = 0x1234;
        re.chunkCount = 1;
        re.relativePath = "r.0.0.0.avrgn";
        ix.add(re);
        std::string why;
        check(writeIndex((dir / "world.ocindex").string(), ix, &why), "the stale index is written: " + why);

        ChunkWorldSettings ws;
        ws.worldDir = dir.string();
        ws.generator.worldSeed = 5;
        {
            g_verWarn = g_seedWarn = 0;
            setLogSink(&captureWarns, nullptr);
            ChunkWorld cw;
            const bool ok = cw.open(ws, &why);
            setLogSink(nullptr, nullptr);
            check(ok, "open succeeds on a directory whose index disagrees: " + why);
            check(g_verWarn == 1, "a generator version mismatch is named");
            check(g_seedWarn == 1, "a world seed mismatch is named");
        }

        ix.generatorVersion = ws.generator.generatorVersion;
        ix.worldSeed = ws.generator.worldSeed;
        check(writeIndex((dir / "world.ocindex").string(), ix, &why), "the matching index is written: " + why);
        {
            g_verWarn = g_seedWarn = 0;
            setLogSink(&captureWarns, nullptr);
            ChunkWorld cw;
            cw.open(ws, &why);
            setLogSink(nullptr, nullptr);
            check(g_verWarn == 0 && g_seedWarn == 0, "an index made by this generator and seed is not complained about");
        }
        std::filesystem::remove_all(dir, ec);
    }

    // ---- (7) the body a host creates is kept, and handed back on eviction -------------------------------------
    {
        clearWorld(w);
        GridSource src;
        src.withBody = true;
        BodyRegistry bodies;
        ChunkStreamer st;
        StreamSettings s;
        s.loadRadius = 1; s.evictRadius = 3; s.verticalRadius = 0;
        s.loadBudget = 0; s.evictBudget = 0;
        s.leadSeconds = 0.0f;
        st.setSettings(s);
        st.setSource(&src);
        i32 nextBody = 1;
        int created = 0;
        st.restoreOptions().createBody = [&](scene::Entity, const Vec3&, const Vec3&) { ++created; return nextBody++; };

        std::vector<i32> freed;
        st.setSources({Vec3{0.0f, 0.0f, 50.0f}});
        const StreamStats a = st.update(w, bodies, &freed, OriginCm{0, 0});
        w.flush();
        check(a.residentEntities == 9 && created == 9, "nine chunks, nine bodies asked for");
        check(bodies.size() == 9, "...and all nine are attached to their entities (restore keeps createBody's return)");

        // Far enough that every chunk is past the evict radius, and where the source has nothing to load.
        src.xHi = 10;
        st.setSources({Vec3{600.0f * 1600.0f, 0.0f, 50.0f}});
        const StreamStats b = st.update(w, bodies, &freed, OriginCm{0, 0});
        w.flush();
        check(b.evictedThisUpdate >= 9, "moving away evicts the old window");
        std::set<i32> ids(freed.begin(), freed.end());
        check(freed.size() == 9 && ids.size() == 9, "eviction hands back each of the nine bodies once");
        check(bodies.size() == 0, "...and the registry is empty again");
        std::vector<i32> drop;
        st.unloadAll(w, bodies, drop);
        w.flush();
        clearWorld(w);
    }

    AVER_INFO("ChunkOriginTest: {} checks, {} failed", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
