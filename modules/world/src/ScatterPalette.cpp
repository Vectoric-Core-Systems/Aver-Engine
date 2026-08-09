#include "aver/world/ScatterPalette.hpp"

#if AVER_MODULE_SCENE

#  include "aver/platform/FileSystem.hpp"

#  include <limits>

namespace aver::world {
namespace {

// f64 -> f32, CLAMPED rather than cast verbatim. OcScatterSpecies's unbounded sentinel is f64's own
// max, which narrows to +-inf under a plain static_cast -- not undefined behaviour is not the bar
// here; the bar is landing on ScatterSpecies's OWN sentinel (f32's max) so a caller comparing against
// it sees exactly what a species declared with no `density` clause at all would produce.
f32 narrowDensity(f64 v, bool isMin) {
    const f32 lim = std::numeric_limits<f32>::max();
    if (isMin && v <= -static_cast<f64>(lim)) return -lim;
    if (!isMin && v >= static_cast<f64>(lim)) return lim;
    return static_cast<f32>(v);
}

} // namespace

bool buildScatterPalette(const std::vector<fmt::OcScatterSpecies>& records,
                          const std::string& contentDir,
                          std::vector<ScatterSpecies>& out,
                          std::vector<std::string>& errors) {
    out.clear();
    errors.clear();

    for (usize i = 0; i < records.size(); ++i) {
        const fmt::OcScatterSpecies& r = records[i];
        const std::string tag = "SCATTER species " + std::to_string(i) +
                                 " (mesh '" + r.meshPath + "')";
        bool ok = true;

        if (r.meshPath.empty()) {
            errors.push_back(tag + ": no mesh given");
            ok = false;
        } else if (!contentDir.empty() && !fileExists(contentDir + "/" + r.meshPath)) {
            errors.push_back(tag + ": mesh does not exist under content root '" + contentDir + "'");
            ok = false;
        }
        if (r.weight <= 0.0) {
            errors.push_back(tag + ": weight must be > 0, got " + std::to_string(r.weight));
            ok = false;
        }
        if (r.scaleMin > r.scaleMax) {
            errors.push_back(tag + ": scaleMin (" + std::to_string(r.scaleMin) +
                              ") is above scaleMax (" + std::to_string(r.scaleMax) + ")");
            ok = false;
        }
        if (r.densityMin > r.densityMax) {
            errors.push_back(tag + ": densityMin (" + std::to_string(r.densityMin) +
                              ") is above densityMax (" + std::to_string(r.densityMax) + ")");
            ok = false;
        }

        if (!ok) continue;   // reported above; the rest of the palette still builds

        ScatterSpecies sp;
        sp.meshPath = r.meshPath;
        sp.material = r.material;
        sp.weight = static_cast<f32>(r.weight);
        sp.scaleMin = static_cast<f32>(r.scaleMin);
        sp.scaleMax = static_cast<f32>(r.scaleMax);
        sp.randomizeYaw = r.randomizeYaw;
        sp.densityMin = narrowDensity(r.densityMin, true);
        sp.densityMax = narrowDensity(r.densityMax, false);
        sp.collisionRadiusCm = static_cast<f32>(r.collisionRadiusCm);
        out.push_back(std::move(sp));
    }

    return errors.empty();
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
