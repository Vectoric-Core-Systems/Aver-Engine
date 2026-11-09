// PCG output sinks and per-cell partitioned generation: cells equal the whole, ownership is exclusive, dirty cells
// regenerate alone, the instance table / chunk payload / actor sinks, GeneratedChunkSource driven by a graph (with
// persistChunk's regenerate-equals-persisted rule), and the PCGVOLUME graph= record. Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/PcgSinks.hpp"
#include "aver/world/RegionFile.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <tuple>

using namespace aver;
using namespace aver::world;

static int g_checks = 0, g_failures = 0;
static std::string why;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

using Key = std::tuple<f32, f32, f32, i32>;
static std::vector<Key> keys(const PcgResult& r, const PcgBox* clip = nullptr) {
    std::vector<Key> v;
    for (const auto& g : r.groups)
        for (const auto& i : g.items)
            if (!clip || clip->containsXY(i.xf.position)) v.emplace_back(i.xf.position.x, i.xf.position.y, i.xf.position.z, i.seed);
    std::sort(v.begin(), v.end());
    return v;
}

static fmt::OcGraphData forestGraph(const char* pruneMode, const char* radius, bool jitter) {
    fmt::OcGraphData g;
    g.domain = "pcg";
    g.name = "forest";
    const auto n = [&](const std::string& id, const std::string& type, std::vector<std::string> p) {
        g.nodes.push_back(pcgMakeNode(id, type, p));
    };
    n("grid", "CreatePointsGrid", {"spacing=60"});
    n("noise", "DensityNoise", {"cell=900", "octaves=3"});
    n("dens", "DensityFilter", {"min=0.45"});
    n("prune", "SelfPrune", {std::string("radius=") + radius, std::string("mode=") + pruneMode});
    n("spawn", "WeightedMeshSpawner", {"meshes=Meshes/pine.ocmesh:2,Meshes/fir.ocmesh:1", "material=M_Tree"});
    g.links.push_back({"grid", "out", "noise", "in"});
    g.links.push_back({"noise", "out", "dens", "in"});
    g.links.push_back({"dens", "inside", "prune", "in"});
    if (jitter) {
        n("jit", "TransformPoints", {"posMin=-25,-25,0", "posMax=25,25,0"});
        g.links.push_back({"prune", "out", "jit", "in"});
        g.links.push_back({"jit", "out", "spawn", "in"});
    } else {
        g.links.push_back({"prune", "out", "spawn", "in"});
    }
    return g;
}

static std::shared_ptr<PcgProgram> programFor(fmt::OcGraphData g, i32 seed = 11) {
    auto p = std::make_shared<PcgProgram>();
    p->graph = std::make_shared<const fmt::OcGraphData>(std::move(g));
    p->seed = seed;
    return p;
}

struct FlatLand final : ILandscapeQuery {
    f32 base = 0.0f;
    bool heightAt(f64, f64, f32& z) const override { z = base; return true; }
};

int main() {
    constexpr i32 kCell = 1600;

    // ---- cells equal the whole --------------------------------------------------------------------------------------------
    {
        const auto prog = programFor(forestGraph("local", "90", false));
        const PcgCellGenerator gen(prog, kCell);
        check(gen.margin() == 90.0f, "the cell margin is the graph's influence radius (local prune: one radius)");

        const PcgBox area{Vec3{0, 0, -1e5f}, Vec3{3 * kCell, 3 * kCell, 1e5f}};
        PcgBox big = area;
        big.min.x = big.min.y = -gen.margin();
        big.max.x = big.max.y = 3 * kCell + gen.margin();
        const PcgResult whole = gen.generateBox(big);
        const std::vector<Key> expect = keys(whole, &area);
        check(expect.size() > 200, "the fixture forest has plenty of trees (" + std::to_string(expect.size()) + ")");

        std::vector<Key> got;
        std::set<std::pair<i32, i32>> order;
        bool disjoint = true;
        for (i32 cy = 2; cy >= 0; --cy)          // deliberately not raster order
            for (i32 cx = 0; cx < 3; ++cx) {
                const PcgResult r = gen.generate(cx, cy);
                const PcgBox own = gen.cellBox(cx, cy);
                for (const auto& k : keys(r)) {
                    disjoint &= own.containsXY(Vec3{std::get<0>(k), std::get<1>(k), 0});
                    got.push_back(k);
                }
            }
        std::sort(got.begin(), got.end());
        check(disjoint, "every point a cell returns lies inside that cell");
        check(std::adjacent_find(got.begin(), got.end()) == got.end(), "no point is produced by two cells");
        check(got == expect, "the nine cells together equal one whole-volume run, point for point and seed for seed");

        PcgCache cache;
        check(keys(gen.generate(1, 1, &cache)) == keys(gen.generate(1, 1)), "a cell is the same with or without the node cache");
    }

    // ---- greedy prune: separated across seams -------------------------------------------------------------------------------
    {
        const auto prog = programFor(forestGraph("greedy", "90", false));
        const PcgCellGenerator gen(prog, kCell);
        std::vector<Key> all;
        for (i32 cy = 0; cy < 3; ++cy)
            for (i32 cx = 0; cx < 3; ++cx) for (const Key& k : keys(gen.generate(cx, cy))) all.push_back(k);
        bool spaced = true;
        for (usize i = 0; i < all.size(); ++i)
            for (usize j = i + 1; j < all.size(); ++j) {
                const f32 dx = std::get<0>(all[i]) - std::get<0>(all[j]), dy = std::get<1>(all[i]) - std::get<1>(all[j]);
                if (dx * dx + dy * dy < 90.0f * 90.0f) spaced = false;
            }
        check(!all.empty() && spaced, "greedy pruning keeps every pair at least a radius apart, cell seams included");
    }

    // ---- ownership of a jittered point ----------------------------------------------------------------------------------------
    {
        const auto prog = programFor(forestGraph("local", "90", true));
        const PcgCellGenerator gen(prog, kCell);
        check(gen.margin() == 90.0f + 25.0f, "jitter adds its amplitude to the margin");
        std::vector<Key> got;
        for (i32 cy = 0; cy < 2; ++cy)
            for (i32 cx = 0; cx < 2; ++cx) for (const Key& k : keys(gen.generate(cx, cy))) got.push_back(k);
        std::sort(got.begin(), got.end());
        check(std::adjacent_find(got.begin(), got.end()) == got.end(), "points jittered across a cell edge still belong to exactly one cell");
    }

    // ---- dirty cells -------------------------------------------------------------------------------------------------------
    {
        auto prog = programFor(forestGraph("local", "90", false));
        PcgCellStore store(prog, kCell);
        for (i32 cy = 0; cy < 3; ++cy) for (i32 cx = 0; cx < 3; ++cx) store.get(cx, cy);
        check(store.generateCalls() == 9 && store.cellCount() == 9, "nine cells generated once each");
        for (i32 cy = 0; cy < 3; ++cy) for (i32 cx = 0; cx < 3; ++cx) store.get(cx, cy);
        check(store.generateCalls() == 9, "asking again regenerates nothing");

        const PcgBox edit{Vec3{kCell + 300, kCell + 300, 0}, Vec3{kCell + 400, kCell + 400, 0}};
        const u32 marked = store.invalidateBox(edit);
        check(marked == 1 && !store.isCurrent(1, 1) && store.isCurrent(0, 0), "a change in the middle of one cell marks only that cell");
        for (i32 cy = 0; cy < 3; ++cy) for (i32 cx = 0; cx < 3; ++cx) store.get(cx, cy);
        check(store.generateCalls() == 10, "and only that cell regenerates");

        const PcgBox seam{Vec3{kCell - 10, 100, 0}, Vec3{kCell + 10, 200, 0}};
        check(store.invalidateBox(seam) == 2, "a change on a seam marks both neighbours (the margin reaches across)");

        // Editing the graph: every cell is stale, regenerated lazily.
        fmt::OcGraphData edited = *prog->graph;
        edited.nodes[2].extraTokens = {"min=0.6"};
        store.setProgram(programFor(edited));
        check(!store.isCurrent(0, 0) && !store.isCurrent(2, 2), "after a graph edit no cell is current");
        const usize before = keys(store.get(0, 0)).size();
        check(store.generateCalls() == 11 + 0 || store.generateCalls() == 12, "a cell regenerates only when asked for");
        check(before < keys(PcgCellGenerator(prog, kCell).generate(0, 0)).size(), "and reflects the edited graph (a stricter density threshold keeps fewer trees)");
        check(store.isCurrent(0, 0) && !store.isCurrent(2, 2), "other cells stay stale until asked");

        auto env = programFor(*prog->graph);
        env->environmentStamp = 5;
        PcgCellStore store2(prog, kCell);
        store2.get(0, 0);
        store2.setProgram(env);
        check(!store2.isCurrent(0, 0), "a changed environment stamp (landscape edit) makes cells stale too");
    }

    // ---- the instance table sink ----------------------------------------------------------------------------------------------
    {
        const auto prog = programFor(forestGraph("local", "90", false));
        const PcgCellGenerator gen(prog, kCell);
        const PcgResult r = gen.generate(0, 0);
        fmt::OcInstanceData flat;
        check(pcgToInstanceData(r, flat, 0.0f, {}, &why) && flat.valid(), "an .ocinst table builds and validates: " + why);
        check(flat.groups.size() == r.groups.size() && flat.transforms.size() == r.instanceCount() * 12, "one 12-float row per instance");
        check(flat.groups[0].asset == "Meshes/pine.ocmesh" || flat.groups[0].asset == "Meshes/fir.ocmesh", "groups are named by mesh");
        const Mat4 m = r.groups[0].items[0].xf.toMatrix();
        check(flat.transforms[9] == m.m[3][0] && flat.transforms[10] == m.m[3][1] && flat.transforms[11] == m.m[3][2],
              "the translation is in row 3 of the row-vector matrix, the .ocinst convention");
        fmt::OcInstanceData celled;
        check(pcgToInstanceData(r, celled, 400.0f, {0.0f, 0.0f}, &why) && celled.valid() && celled.cellCm == 400.0f && !celled.cells.empty(),
              "and sorts into spatial cells when asked: " + why);
        u32 rows = 0;
        for (const auto& run : celled.runs) rows += run.count;
        check(rows == r.instanceCount(), "no instance is lost to cell sorting");
        std::vector<u8> bytes;
        fmt::OcInstanceData back;
        check(fmt::writeOcInstances(celled, bytes, &why) && fmt::parseOcInstances(bytes.data(), bytes.size(), back, &why) &&
                  back.transforms == celled.transforms, "the table round-trips through the AVR1 writer");
    }

    // ---- chunk payload and GeneratedChunkSource ---------------------------------------------------------------------------------
    {
        const auto prog = programFor(forestGraph("local", "90", false));
        GeneratorSettings gs;
        gs.chunkSizeCm = kCell;
        gs.worldSeed = 99;
        gs.pcgProgram = prog;
        gs.generatorVersion = pcgGeneratorVersion(*prog);
        check(gs.generatorVersion >= 0x30000000u, "graph worlds record a generatorVersion in their own range");
        fmt::OcGraphData other = *prog->graph;
        other.nodes[3].extraTokens = {"radius=95", "mode=local"};
        check(pcgGeneratorVersion(*programFor(other)) != gs.generatorVersion, "and editing the graph changes it (stale saves are detectable)");

        GeneratedChunkSource src(gs);
        ChunkPayload p0, p1;
        check(src.has(ChunkCoord{1, 2, 0}) && src.load(ChunkCoord{1, 2, 0}, p0, &why), "a graph chunk loads");
        check(!p0.entities.empty(), "with trees in it");
        bool inside = true, mesh = true;
        std::set<std::string> names;
        for (const auto& e : p0.entities) {
            inside &= e.local.position.x >= 0 && e.local.position.x < kCell && e.local.position.y >= 0 && e.local.position.y < kCell;
            mesh &= e.hasMesh && e.material == "M_Tree";
            names.insert(e.name);
        }
        check(inside, "entity positions are chunk-local");
        check(mesh && names.size() == p0.entities.size(), "each entity has a mesh and a unique seed-derived name");

        GeneratedChunkSource src2(gs);
        src2.load(ChunkCoord{1, 2, 0}, p1, &why);
        check(encodeChunk(p0) == encodeChunk(p1), "a second source generates identical chunk bytes");
        ChunkPayload n0, n1;
        src.load(ChunkCoord{0, 0, 0}, n0, &why);
        src2.load(ChunkCoord{1, 2, 0}, n1, &why);
        src2.load(ChunkCoord{0, 0, 0}, n1, &why);
        check(encodeChunk(n0) == encodeChunk(n1), "generation order does not change a chunk");
        check(!src.has(ChunkCoord{0, 0, 1}), "chunks off the surface layer stay empty");

        // Without a program the palette generator is untouched.
        GeneratorSettings plain = gs;
        plain.pcgProgram = nullptr;
        plain.generatorVersion = 2;
        plain.samplesPerAxis = 5; plain.threshold = 0.55f;
        GeneratorSettings plain2 = plain;
        GeneratedChunkSource a(plain), b(plain2);
        ChunkPayload pa, pb;
        a.load(ChunkCoord{0, 0, 0}, pa, &why);
        b.load(ChunkCoord{0, 0, 0}, pb, &why);
        check(!pa.entities.empty() && encodeChunk(pa) == encodeChunk(pb) && pa.entities[0].name.rfind("gen_", 0) == 0,
              "with pcgProgram unset the palette generator still produces its gen_ entities");

        // persistChunk: regenerate equals persisted.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-pcg-partition-test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        RegionWriteDesc desc;
        desc.coord = RegionCoord{0, 0, 0};
        desc.chunkSizeCm = kCell;
        desc.levelId = 1;
        desc.generatorVersion = gs.generatorVersion;
        RegionWriter wr;
        check(wr.open((dir / "pcg.avrgn").string(), desc, &why), "a save region opens: " + why);
        const ChunkCoord c{1, 2, 0};
        check(persistChunk(wr, src, c, p0, &why) == PersistOutcome::Unchanged && wr.header().chunkCount == 0,
              "an unedited graph chunk persists to nothing");
        ChunkPayload edited = p0;
        edited.entities.front().local.position.x += 3.0f;
        check(persistChunk(wr, src, c, edited, &why) == PersistOutcome::Stored, "an edited one is stored as an override");
        check(persistChunk(wr, src, c, p0, &why) == PersistOutcome::OverrideRemoved, "putting it back removes the override");
        wr.close();
        std::filesystem::remove_all(dir, ec);
    }

    // ---- actor sink ---------------------------------------------------------------------------------------------------------------
    {
        fmt::OcGraphData g;
        g.domain = "pcg";
        g.nodes.push_back(pcgMakeNode("grid", "CreatePointsGrid", {"spacing=400"}));
        g.nodes.push_back(pcgMakeNode("act", "ActorSpawner", {"class=Crate"}));
        g.links.push_back({"grid", "out", "act", "in"});
        const PcgCellGenerator gen(programFor(g), kCell);
        const PcgResult r = gen.generate(0, 0);
        const auto owned = pcgActorsIn(r, gen.cellBox(0, 0));
        check(owned.size() == 16 && owned[0].actorClass == "Crate", "actor spawns are produced per owned point (4 x 4 in a 1600 cm cell)");
        const auto twice = pcgActorsIn(gen.generate(0, 0), gen.cellBox(0, 0));
        bool same = twice.size() == owned.size();
        for (usize i = 0; same && i < owned.size(); ++i) same &= owned[i].seed == twice[i].seed;
        check(same, "in a canonical, repeatable order");
        check(pcgActorsIn(r, PcgBox{Vec3{100000, 0, 0}, Vec3{101600, 1600, 0}}).empty(), "and none belong to a far-away box");
    }

    // ---- landscape through a program -------------------------------------------------------------------------------------------------
    {
        fmt::OcGraphData g;
        g.domain = "pcg";
        g.nodes.push_back(pcgMakeNode("s", "SurfaceSampler", {"spacing=400"}));
        g.nodes.push_back(pcgMakeNode("m", "WeightedMeshSpawner", {"meshes=Meshes/rock.ocmesh"}));
        g.links.push_back({"s", "out", "m", "in"});
        auto prog = programFor(g);
        auto land = std::make_shared<FlatLand>();
        land->base = 250.0f;
        prog->landscape = land;
        const PcgResult r = PcgCellGenerator(prog, kCell).generate(0, 0);
        bool z = r.instanceCount() == 16;
        for (const auto& i : r.groups[0].items) z &= i.xf.position.z == 250.0f;
        check(z, "a program's landscape reaches the surface sampler");
        const ChunkPayload cp = pcgToChunkPayload(r, ChunkCoord{0, 0, 0}, kCell);
        bool lz = cp.entities.size() == 16;
        for (const auto& e : cp.entities) lz &= e.local.position.z == 250.0f;
        check(lz, "and chunk-local Z is height minus the chunk origin");
    }

    // ---- PCGVOLUME graph= -------------------------------------------------------------------------------------------------------------
    {
        fmt::OcWorldData w;
        std::string err;
        const std::string text =
            "OCWORLD 1\nNAME T\n"
            "PCGVOLUME name Sky seed 5 cell 1600 octaves 4 floor 0 bias 1 infinite\n"
            "PCGVOLUME name Forest seed 12 cell 1600 octaves 4 floor 0 bias 1 radius 4 graph Content/PCG/PG_Forest.ocgraph transient bounds 0 0 -500 6400 6400 500\n";
        check(fmt::parseOcworld(text, w, &err), "a level with a graph volume parses: " + err);
        check(w.pcgVolumes.size() == 2 && w.pcgVolumes[0].graph.empty() && !w.pcgVolumes[0].transient,
              "the Sky volume carries no graph and behaves as before");
        const auto& f = w.pcgVolumes[1];
        check(f.graph == "Content/PCG/PG_Forest.ocgraph" && f.transient && !f.infinite && f.seed == 12 && f.boundsMax[0] == 6400.0 && f.radiusChunks == 4,
              "the graph volume keeps its graph, seed, transient flag and bounds");
        const std::string back = fmt::writeOcworld(w);
        fmt::OcWorldData w2;
        check(fmt::parseOcworld(back, w2, &err) && w2.pcgVolumes[1].graph == f.graph && w2.pcgVolumes[1].transient && fmt::writeOcworld(w2) == back,
              "and writes back identically");
        fmt::OcWorldData legacy;
        fmt::parseOcworld("OCWORLD 1\nNAME T\nPCGVOLUME name Floor seed 8 cell 1600 octaves 3 floor 0.3 bias 1 infinite\n", legacy, &err);
        check(fmt::writeOcworld(legacy).find("graph") == std::string::npos, "a volume without a graph writes no graph token");
    }

    // ---- the shipped sample graph ----------------------------------------------------------------------------------------------------
    {
        fmt::OcGraphData sample;
        check(fmt::loadOcgraph(AVER_PCG_SAMPLE, sample, &why), "the template's sample graph loads: " + why);
        check(pcgIsGraph(sample) && pcgValidate(sample).empty(), "and is a valid pcg graph");
        struct Rolling final : ILandscapeQuery {
            bool heightAt(f64 x, f64 y, f32& z) const override { z = static_cast<f32>(200.0 * std::sin(x * 0.0006) + 100.0 * std::cos(y * 0.0009)); return true; }
        };
        auto prog = programFor(sample, 3);
        prog->landscape = std::make_shared<Rolling>();
        const PcgCellGenerator gen(prog, 6400);
        const PcgResult r = gen.generate(0, 0);
        check(r.ok && r.instanceCount() > 30 && r.groups.size() == 2, "it plants trees of both kinds on a rolling landscape (" + std::to_string(r.instanceCount()) + ")");
        check(keys(r) == keys(gen.generate(0, 0)), "and plants them identically every time");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
