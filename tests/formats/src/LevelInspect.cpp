// Command-line tool: loads a .ocworld/.ocmap through the LIVE parser (parseOcworld/loadOcworld,
// modules/formats/src/OcWorld.cpp) and reports its PCGVOLUME and SCATTER records.
//
// WHY THIS EXISTS ALONGSIDE FormatTest. FormatTest's own per-file dispatch (testMap) routes a
// ".ocmap" extension through the SEPARATE legacy OcMap.cpp parser, which has no PCGVOLUME or SCATTER
// concept at all -- it would silently report neither, agreeing with a file that in fact declares
// both. A hand-authored level's SCATTER lines get no editor and no compiler; a typo in a keyword is
// swallowed by parseOcworld's own "unknown records are skipped, not failed" contract exactly like any
// other line it does not recognise, so the only way to catch one is to load the file through the
// SAME parser the engine uses and read back what it actually kept.
//
//     LevelInspect.exe "<project>\Content\Maps\Default.ocmap"
#include "aver/formats/OcWorld.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

int main(int argc, char** argv) {
    if (argc < 2) {
        AVER_INFO("usage: LevelInspect <file.ocworld|file.ocmap> [more...]");
        return exitCode(ExitCode::Usage);
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        fmt::OcWorldData w;
        std::string err;
        if (!fmt::loadOcworld(path, w, &err)) {
            AVER_ERROR("=== {} : LOAD FAILED: {} ===", path, err);
            ++failures;
            continue;
        }
        AVER_INFO("=== {} ===", path);
        AVER_INFO("   name='{}'  id=0x{:016X}  placements={}", w.name, w.contentId, w.placements.size());
        for (const fmt::OcPcgVolume& v : w.pcgVolumes) {
            AVER_INFO("   PCGVOLUME '{}': seed={} cell={} octaves={} floor={} bias={} infinite={}",
                      v.name, v.seed, v.cellSizeCm, v.octaves, v.coverageFloor, v.coverageBias, v.infinite);
        }
        AVER_INFO("   scatterSpecies: {}", w.scatterSpecies.size());
        for (usize s = 0; s < w.scatterSpecies.size(); ++s) {
            const fmt::OcScatterSpecies& sp = w.scatterSpecies[s];
            AVER_INFO("     [{}] mesh='{}' material='{}' weight={} scale=[{},{}] density=[{},{}] "
                      "collide={} yaw={}",
                      s, sp.meshPath, sp.material, sp.weight, sp.scaleMin, sp.scaleMax,
                      sp.densityMin, sp.densityMax, sp.collisionRadiusCm, sp.randomizeYaw);
        }
    }
    return exitCode(failures ? ExitCode::Failed : ExitCode::Ok);
}
