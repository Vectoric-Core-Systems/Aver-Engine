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

    // Same mapping SandboxApp.cpp::setChunkStreamingEnabled uses -- ONE GENERATOR PER non-Sky
    // PCGVOLUME, each with its own species subset (SCATTER `volume`) and its own sweep radius
    // (PCGVOLUME `radius`).
    //
    // THIS TOOL IS ONLY WORTH ANYTHING IF IT MIRRORS THAT MAPPING EXACTLY. It used to take the FIRST
    // non-Sky volume and run the WHOLE palette against it, which was right when there could only be
    // one field. The moment a level declares several it becomes a lie in both directions at once:
    // it would report the canopy's pines as though they were scattered by the floor's field, at the
    // floor's radius, and it would never sweep the canopy's own much larger one. A budget tool that
    // silently diverges from the runtime is worse than no budget tool -- the same argument its
    // `samples` clamp below already carries.
    std::vector<const fmt::OcPcgVolume*> fields;
    for (const fmt::OcPcgVolume& v : w.pcgVolumes)
        if (v.name != "Sky") fields.push_back(&v);
    if (fields.empty()) {
        AVER_ERROR("no non-Sky PCGVOLUME found -- nothing to generate");
        return 1;
    }

    struct SpeciesInfo { usize idx; std::string meshPath; f64 triCount; u64 instances = 0; };

    u64 grandChunks = 0, grandEntities = 0;
    f64 grandTris = 0.0;

    for (usize fi = 0; fi < fields.size(); ++fi) {
        const fmt::OcPcgVolume& v = *fields[fi];

        GeneratorSettings gen;
        gen.worldSeed = static_cast<u64>(static_cast<u32>(v.seed));
        if (v.cellSizeCm > 0.0) gen.featureSizeCm = static_cast<f32>(v.cellSizeCm);
        if (v.octaves > 0) gen.octaves = static_cast<u32>(v.octaves);
        {
            const f64 t = v.coverageFloor < 0.0 ? 0.0 : (v.coverageFloor > 1.0 ? 1.0 : v.coverageFloor);
            gen.threshold = static_cast<f32>(t);
        }
        // samples: the same clamp SandboxApp::setChunkStreamingEnabled applies. It was written
        // before `samples` existed and reported a level asking for 16 as though it had asked for
        // the default 4 -- one sixteenth of the real instance count.
        if (v.samplesPerAxis > 0) {
            constexpr i32 kMaxSamplesPerAxis = 64;
            const i32 n = v.samplesPerAxis > kMaxSamplesPerAxis ? kMaxSamplesPerAxis : v.samplesPerAxis;
            gen.samplesPerAxis = static_cast<u32>(n);
        }

        // Which species this field places, by exactly the runtime's rule: a species naming this
        // volume, plus -- for the FIRST field only -- every species naming no volume at all, and
        // every species naming one the level does not declare.
        std::vector<fmt::OcScatterSpecies> mine;
        for (const fmt::OcScatterSpecies& sp : w.scatterSpecies) {
            bool named = false;
            if (!sp.volume.empty())
                for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { named = true; break; }
            if (named) { if (sp.volume == v.name) mine.push_back(sp); }
            else if (fi == 0)                     mine.push_back(sp);
        }

        // The field's own sweep radius, so each field is measured over the area it actually covers.
        // The CLI radius stands in when a field declares none, which is every level written before
        // `radius` existed.
        const int r = v.radiusChunks > 0 ? v.radiusChunks : radius;

        AVER_INFO("");
        AVER_INFO("=== PCGVOLUME '{}': seed={} featureSize={:.0f}cm octaves={} threshold={:.2f} "
                  "samples={}/axis radius={} ===",
                  v.name, gen.worldSeed, gen.featureSizeCm, gen.octaves, gen.threshold,
                  gen.samplesPerAxis, r);

        if (mine.empty()) {
            AVER_INFO("   no SCATTER species bound to this field -- not streamed");
            continue;
        }

        std::vector<std::string> paletteErrors;
        if (!buildScatterPalette(mine, contentDir, gen.palette, paletteErrors)) {
            for (const std::string& e : paletteErrors) AVER_WARN("   [palette] {}", e);
        }
        AVER_INFO("   palette: {} of {} SCATTER record(s) bound to this field validated",
                  gen.palette.size(), mine.size());
        if (gen.palette.empty()) {
            AVER_WARN("   palette is empty after validation -- this field would scatter nothing");
            continue;
        }

        // Per-species mesh hash -> (index, triangle count). Loaded once per unique mesh; a landmark
        // mesh in the multi-million-triangle range is read exactly once regardless of instance count.
        std::map<u64, SpeciesInfo> byHash;
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
        const int r2 = r * r;
        for (int cy = -r; cy <= r; ++cy) {
            for (int cx = -r; cx <= r; ++cx) {
                if (cx * cx + cy * cy > r2) continue;  // circular selection, not the full square
                ++chunkCount;
                const ChunkPayload p = src.generate(ChunkCoord{cx, cy, 0});
                for (const PayloadEntity& e : p.entities) {
                    if (!e.hasMesh) continue;
                    auto it = byHash.find(e.mesh);
                    if (it == byHash.end()) continue;  // e.g. a mesh hash not in this field's palette
                    ++it->second.instances;
                    totalTris += it->second.triCount;
                    ++entityCount;
                }
            }
        }

        AVER_INFO("   --- radius={} chunks (circular, {} selected), surfaceZ=0 ---", r, chunkCount);
        for (const auto& [hash, info] : byHash) {
            AVER_INFO("      {} : {} instance(s), {:.0f} tris total", info.meshPath, info.instances,
                      static_cast<f64>(info.instances) * info.triCount);
        }
        AVER_INFO("   FIELD '{}': {} entities across {} chunks, {:.0f} resident triangles",
                  v.name, entityCount, chunkCount, totalTris);

        grandChunks   += chunkCount;
        grandEntities += entityCount;
        grandTris     += totalTris;
    }

    AVER_INFO("");
    AVER_INFO("TOTAL across {} field(s): {} entities across {} chunk-loads, {:.0f} resident triangles "
              "(LOD0, no distance-based LOD selection applied -- a real render would be lower, not higher)",
              fields.size(), grandEntities, grandChunks, grandTris);
    return 0;
}
