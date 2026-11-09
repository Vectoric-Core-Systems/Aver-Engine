// Editor: the PCG Graph tab's connection to the open level (WAVE2_UE W3 step 6). The tab asks for the landscape, publishes a baked table and cleans it
// up again through PcgEditorHost; this file is where those answers come from. Publish writes Content/Foliage/<graph>_pcg.ocinst (cell-sorted), lists it in the
// level's FOLIAGE files and shows it through the level's extra foliage tables, exactly like a layer scatter; Clean up reverses all three.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE && AVER_MODULE_LANDSCAPE

#include "PcgGraphEditor.hpp"
#include "VegetationGenerate.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/PcgGraph.hpp"

#include <algorithm>
#include <filesystem>

namespace aver {

namespace {

std::string outputRelFor(const std::string& graphPath) { return "Foliage/" + std::filesystem::path(graphPath).stem().string() + "_pcg.ocinst"; }

}  // namespace

void SandboxApp::installPcgEditorHost() {
    editor::PcgEditorHost h;
    h.landscape = [this]() -> std::shared_ptr<const world::ILandscapeQuery> {
        if (landscape_.sectionCount() == 0) return nullptr;
        std::vector<const fmt::OcLandData*> sections;
        for (u32 i = 0; i < landscape_.sectionCount(); ++i) sections.push_back(&landscape_.sectionData(i));
        return std::make_shared<editor::LandSectionsQuery>(std::move(sections));
    };
    h.landscapeExtent = [this](f32 out[4]) {
        bool any = false;
        out[0] = out[1] = 1.0e30f; out[2] = out[3] = -1.0e30f;
        for (u32 i = 0; i < landscape_.sectionCount(); ++i) {
            const fmt::OcLandData& d = landscape_.sectionData(i);
            if (!d.valid()) continue;
            out[0] = std::min(out[0], d.boundsMin[0]); out[1] = std::min(out[1], d.boundsMin[1]);
            out[2] = std::max(out[2], d.boundsMax[0]); out[3] = std::max(out[3], d.boundsMax[1]);
            any = true;
        }
        return any;
    };
    h.publish = [this](const fmt::OcInstanceData& table, const std::vector<std::string>& materials, const std::string& graphPath, std::string* why) -> std::string {
        if (!project_.valid() || levelPath_.empty()) { if (why) *why = "open a project and a level first"; return {}; }
        const std::string rel = outputRelFor(graphPath);
        std::error_code ec;
        std::filesystem::create_directories((std::filesystem::path(project_.contentDir()) / rel).parent_path(), ec);
        if (!fmt::saveOcInstances(project_.contentDir() + "/" + rel, table, why)) return {};
        if (std::find(lvl_.levelHeader_.foliageFiles.begin(), lvl_.levelHeader_.foliageFiles.end(), rel) == lvl_.levelHeader_.foliageFiles.end()) {
            lvl_.levelHeader_.foliageFiles.push_back(rel);
            markLevelUnsaved();
        }
#  if AVER_MODULE_VOXI
        if (rset_.voxiAttached_) {
            fmt::OcInstanceData copy = table;
            levelFoliage_.setExtraTable(fnv1a64(std::string_view(rel)), std::move(copy), materials);
        }
#  else
        (void)materials;
#  endif
        return rel;
    };
    h.clean = [this](const std::string& graphPath, std::string* why) {
        if (!project_.valid()) { if (why) *why = "open a project first"; return false; }
        const std::string rel = outputRelFor(graphPath);
        const auto it = std::find(lvl_.levelHeader_.foliageFiles.begin(), lvl_.levelHeader_.foliageFiles.end(), rel);
        const std::string file = project_.contentDir() + "/" + rel;
        std::error_code ec;
        const bool had = it != lvl_.levelHeader_.foliageFiles.end() || std::filesystem::exists(file, ec);
        if (!had) { if (why) *why = "nothing was generated for this graph"; return false; }
        if (it != lvl_.levelHeader_.foliageFiles.end()) { lvl_.levelHeader_.foliageFiles.erase(it); markLevelUnsaved(); }
#  if AVER_MODULE_VOXI
        if (rset_.voxiAttached_) levelFoliage_.clearExtraTable(fnv1a64(std::string_view(rel)));
#  endif
        std::filesystem::remove(file, ec);
        return true;
    };
    editor::setPcgEditorHost(std::move(h));
}

// A starter `DOMAIN pcg` graph: a grid into a density filter into a spawner, so the first Run shows something.
void SandboxApp::cbCreatePcgGraph() {
    const std::filesystem::path target = cbFreeAssetPath("NewPcgGraph", ".ocgraph");
    if (target.empty()) return;
    fmt::OcGraphData g;
    g.name = target.stem().string();
    std::string grid, keep, spawn;
    editor::pcged::addNode(g, "CreatePointsGrid", 0, 0, &grid);
    editor::pcged::addNode(g, "DensityNoise", 220, 0, &keep);
    editor::pcged::addNode(g, "WeightedMeshSpawner", 440, 0, &spawn);
    g.links.push_back({grid, "out", keep, "in"});
    g.links.push_back({keep, "out", spawn, "in"});
    std::string why;
    if (!writeFileTextAtomic(target.string(), fmt::writeOcgraph(g, ""))) {
        drawerUi_.cbStatus_ = "Could not write " + target.filename().string();
        return;
    }
    cbAdoptNewAsset(target);
}

}  // namespace aver

#endif
