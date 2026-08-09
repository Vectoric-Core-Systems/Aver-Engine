// ScatterBudgetTool -- not a test. A CLI that runs the REAL generator against a level's REAL
// SCATTER records and REAL cooked meshes, and reports a measured resident-triangle count for a
// realistic streaming radius. Exists because Sandbox.exe cannot answer this question yet: the
// palette wiring into GeneratorSettings::palette (see modules/world/include/aver/world/
// ScatterPalette.hpp) has a home in sandbox/src/SandboxApp.cpp that has not landed, so a screenshot
// through the editor still renders the OLD five-species hardcoded palette, not a level's own SCATTER
// data. This tool exercises the SAME GeneratedChunkSource/buildScatterPalette code the editor will
// call once that wiring exists, so its numbers are a real measurement of what the level currently
// declares -- not a guess, and not dependent on the sandbox/src edit landing.
//
// It reproduces the PCGVOLUME -> GeneratorSettings mapping exactly as SandboxApp.cpp's
// setChunkStreamingEnabled does it (read there, not duplicated by invention): the first non-"Sky"
// PCGVOLUME supplies worldSeed/featureSizeCm/octaves, and coverageFloor clamped to [0,1] supplies
// threshold. chunkSizeCm is left at GeneratorSettings' own default (1600cm), matching every level in
// this project pinning PCGVOLUME cell to the chunk size on purpose (see Default.ocmap's own comment
// on why).
//
// Chunk selection is a flat circle of the given radius (Euclidean, in chunk units) around the
// origin chunk on the surface Z layer -- not necessarily the exact enumeration order
// ChunkStreamer uses internally (that lives outside this tool's scope), but the same shape of
// selection and, more importantly, the exact same per-chunk generation code, so the triangle sum is
// real work done by the real generator, not an estimate.
//
//     ScatterBudgetTool.exe "<project>\Content\Maps\Default.ocmap" "<project>\Content" [radius]
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/ScatterPalette.hpp"

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

int main(int argc, char** argv) {
    if (argc < 3) {
        AVER_INFO("usage: ScatterBudgetTool <file.ocworld|file.ocmap> <contentDir> [radiusInChunks=3]");
        return 1;
    }
    const std::string levelPath = argv[1];
    const std::string contentDir = argv[2];
    const int radius = argc > 3 ? std::atoi(argv[3]) : 3;

    fmt::OcWorldData w;
    std::string err;
    if (!fmt::loadOcworld(levelPath, w, &err)) {
        AVER_ERROR("LOAD FAILED: {}", err);
        return 1;
    }
    AVER_INFO("=== {} ===", levelPath);
    AVER_INFO("   name='{}'  placements={}  scatterSpecies={}", w.name, w.placements.size(), w.scatterSpecies.size());

    // Same mapping SandboxApp.cpp::setChunkStreamingEnabled uses: first non-Sky PCGVOLUME.
    GeneratorSettings gen;
    bool foundVolume = false;
    for (const fmt::OcPcgVolume& v : w.pcgVolumes) {
        if (v.name == "Sky") continue;
        gen.worldSeed = static_cast<u64>(static_cast<u32>(v.seed));
        if (v.cellSizeCm > 0.0) gen.featureSizeCm = static_cast<f32>(v.cellSizeCm);
        if (v.octaves > 0) gen.octaves = static_cast<u32>(v.octaves);
        const f64 t = v.coverageFloor < 0.0 ? 0.0 : (v.coverageFloor > 1.0 ? 1.0 : v.coverageFloor);
        gen.threshold = static_cast<f32>(t);
        AVER_INFO("   PCGVOLUME '{}': seed={} featureSize={:.0f}cm octaves={} threshold={:.2f}",
                  v.name, gen.worldSeed, gen.featureSizeCm, gen.octaves, gen.threshold);
        foundVolume = true;
        break;
    }
    if (!foundVolume) {
        AVER_ERROR("no non-Sky PCGVOLUME found -- nothing to generate");
        return 1;
    }

    std::vector<std::string> paletteErrors;
    if (!buildScatterPalette(w.scatterSpecies, contentDir, gen.palette, paletteErrors)) {
        for (const std::string& e : paletteErrors) AVER_WARN("   [palette] {}", e);
    }
    AVER_INFO("   palette: {} of {} SCATTER record(s) validated against '{}'",
              gen.palette.size(), w.scatterSpecies.size(), contentDir);
    if (gen.palette.empty()) {
        AVER_ERROR("palette is empty after validation -- nothing would scatter");
        return 1;
    }

    // Per-species mesh hash -> (index, triangle count). Loaded once per unique mesh; a landmark mesh
    // in the multi-million-triangle range is read exactly once regardless of instance count.
    struct SpeciesInfo { usize idx; std::string meshPath; f64 triCount; u64 instances = 0; };
    std::map<u64, SpeciesInfo> byHash;
    f64 totalTrisPerInstanceSum = 0.0;
    for (usize i = 0; i < gen.palette.size(); ++i) {
        const ScatterSpecies& sp = gen.palette[i];
        const u64 h = fnv1a64(sp.meshPath);
        fmt::OcMeshData mesh;
        std::string meshErr;
        f64 tris = 0.0;
        const std::string full = contentDir.empty() ? sp.meshPath : (contentDir + "\\" + sp.meshPath);
        if (fmt::loadOcMesh(full, mesh, &meshErr)) {
            tris = static_cast<f64>(mesh.indices.size() / 3);
        } else {
            AVER_WARN("   [mesh] could not load '{}' for a triangle count: {}", full, meshErr);
        }
        byHash[h] = SpeciesInfo{i, sp.meshPath, tris, 0};
        AVER_INFO("   [{}] {}  weight={:.3f} density=[{:.2f},{:.2f}] collide={:.0f}cm  LOD0 tris={:.0f}",
                  i, sp.meshPath, sp.weight, sp.densityMin, sp.densityMax, sp.collisionRadiusCm, tris);
    }

    GeneratedChunkSource src(gen);

    u64 chunkCount = 0, entityCount = 0;
    f64 totalTris = 0.0;
    const int r2 = radius * radius;
    for (int cy = -radius; cy <= radius; ++cy) {
        for (int cx = -radius; cx <= radius; ++cx) {
            if (cx * cx + cy * cy > r2) continue;  // circular selection, not the full square
            ++chunkCount;
            const ChunkPayload p = src.generate(ChunkCoord{cx, cy, 0});
            for (const PayloadEntity& e : p.entities) {
                if (!e.hasMesh) continue;
                auto it = byHash.find(e.mesh);
                if (it == byHash.end()) continue;  // e.g. the ground plane's own mesh hash, not in the palette
                ++it->second.instances;
                totalTris += it->second.triCount;
                ++entityCount;
            }
        }
    }

    AVER_INFO("--- radius={} chunks (circular, {} selected), surfaceZ=0 ---", radius, chunkCount);
    for (const auto& [hash, info] : byHash) {
        AVER_INFO("   {} : {} instance(s), {:.0f} tris total", info.meshPath, info.instances,
                  static_cast<f64>(info.instances) * info.triCount);
    }
    AVER_INFO("TOTAL: {} entities across {} chunks, {:.0f} resident triangles (LOD0, no distance-based LOD "
              "selection applied -- a real render would be lower, not higher)",
              entityCount, chunkCount, totalTris);
    return 0;
}
