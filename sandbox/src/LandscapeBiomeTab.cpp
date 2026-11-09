// Editor: the Landscape mode's Biomes section (WAVE2_UE W4 step 6, the part that needs no renderer). The project's .ocbiome files as a list with a
// checkbox each, New Biome from a foliage type, a seed and cell size, the guardrail estimate, and Generate Vegetation: the checked biomes run over
// every landscape section into Content/Foliage/<name>.ocinst with its .ocveg record, shown through the level's extra foliage tables and listed in
// the level's FOLIAGE files like a layer scatter. Hand edits recorded in the .ocveg survive a regenerate.
//
// Not here: a mask preview drawn on the terrain, a density heatmap, per-cell regeneration, and a biome entry editor (entries are edited as text).

#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE && AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI

#include "BiomeListModel.hpp"
#include "VegetationGenerate.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace aver {

namespace {

struct BiomeUi {
    std::string root;                    // the content dir the lists were read from
    std::vector<std::string> biomes, foliage;
    std::vector<char> checked;           // parallel to biomes
    int foliagePick = 0;
    int seed = 1;
    int cellChoice = 1;                  // 0 = 32 m, 1 = 64 m, 2 = 128 m
    char outName[64] = "Biomes";
    std::string estimate, status;
    bool estimateStale = true;
};

BiomeUi& biomeUi() {
    static BiomeUi u;
    return u;
}

void rescan(BiomeUi& u, const std::string& contentDir) {
    std::vector<std::string> keep;
    for (usize i = 0; i < u.biomes.size(); ++i) if (u.checked[i]) keep.push_back(u.biomes[i]);
    u.root = contentDir;
    u.biomes = editor::biomes::listFiles(contentDir, ".ocbiome");
    u.foliage = editor::biomes::listFiles(contentDir, ".ocfoliage");
    u.checked.assign(u.biomes.size(), 0);
    for (usize i = 0; i < u.biomes.size(); ++i) u.checked[i] = std::find(keep.begin(), keep.end(), u.biomes[i]) != keep.end();
    u.foliagePick = std::min(u.foliagePick, std::max(0, static_cast<int>(u.foliage.size()) - 1));
    u.estimateStale = true;
}

}  // namespace

void SandboxApp::buildBiomeSection() {
    BiomeUi& u = biomeUi();
    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("Biomes (Generate Vegetation)")) return;
    if (!projSess_.project_.valid()) { ImGui::TextDisabled("Open a project first."); return; }
    const std::string contentDir = projSess_.project_.contentDir();
    if (u.root != contentDir) rescan(u, contentDir);

    std::vector<const fmt::OcLandData*> sections;
    for (u32 i = 0; i < landscape_.sectionCount(); ++i) sections.push_back(&landscape_.sectionData(i));

    if (u.biomes.empty()) ImGui::TextWrapped("No .ocbiome in this project. New Biome makes one from a foliage type; edit its rules (mask, slope, layer, noise, clumping) as text.");
    for (usize i = 0; i < u.biomes.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool on = u.checked[i] != 0;
        if (ImGui::Checkbox(u.biomes[i].c_str(), &on)) { u.checked[i] = on; u.estimateStale = true; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Rescan")) rescan(u, contentDir);

    // New Biome from a foliage type.
    ImGui::BeginDisabled(u.foliage.empty());
    ImGui::SetNextItemWidth(-90.0f * viewState_.dpi_);
    if (ImGui::BeginCombo("##biomefoliage", u.foliage.empty() ? "(no .ocfoliage in the project)" : u.foliage[static_cast<usize>(u.foliagePick)].c_str())) {
        for (int i = 0; i < static_cast<int>(u.foliage.size()); ++i)
            if (ImGui::Selectable(u.foliage[static_cast<usize>(i)].c_str(), u.foliagePick == i)) u.foliagePick = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Biome")) {
        const std::string foliageRel = u.foliage[static_cast<usize>(u.foliagePick)];
        const std::string rel = editor::biomes::freeBiomePath(contentDir, editor::biomes::stemOf(foliageRel));
        std::error_code ec;
        std::filesystem::create_directories((std::filesystem::path(contentDir) / rel).parent_path(), ec);
        std::string why;
        if (fmt::saveOcBiome(contentDir + "/" + rel, editor::biomes::starterBiome(editor::biomes::stemOf(rel), foliageRel), &why)) {
            rescan(u, contentDir);
            for (usize i = 0; i < u.biomes.size(); ++i) if (u.biomes[i] == rel) u.checked[i] = 1;
            u.status = "Created " + rel;
        } else {
            u.status = "Could not create the biome: " + why;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A one-entry biome for the chosen foliage type, growing everywhere.\nNarrow it by editing the .ocbiome text (mask, slope, layer, noise, clump).");

    if (ImGui::InputInt("Seed", &u.seed)) u.estimateStale = true;
    const char* cells[] = {"32 m", "64 m", "128 m"};
    ImGui::SetNextItemWidth(120.0f * viewState_.dpi_);
    if (ImGui::Combo("Cell size", &u.cellChoice, cells, 3)) u.estimateStale = true;
    ImGui::SetNextItemWidth(160.0f * viewState_.dpi_);
    ImGui::InputText("Output name", u.outName, sizeof u.outName);

    editor::BiomeVegetationRequest req;
    for (usize i = 0; i < u.biomes.size(); ++i) if (u.checked[i]) req.biomes.push_back(u.biomes[i]);
    req.seed = u.seed;
    req.cellCm = 3200.0f * static_cast<f32>(1 << u.cellChoice);
    req.ocinstRel = std::string("Foliage/") + (u.outName[0] ? u.outName : "Biomes") + "_vegetation.ocinst";

    if (u.estimateStale) {
        u.estimateStale = false;
        u.estimate.clear();
        if (!req.biomes.empty() && !sections.empty()) {
            const editor::BiomeEstimate est = editor::estimateBiomeVegetation(sections, req, contentDir);
            u.estimate = est.ok ? editor::biomes::estimateLine(est.instancesPerCellMax, est.bytesPerCellMax, est.overCap) : "Cannot estimate: " + est.error;
            for (const std::string& w : est.warnings) u.estimate += "\n" + w;
        }
    }
    if (!u.estimate.empty()) ImGui::TextWrapped("%s", u.estimate.c_str());

    ImGui::BeginDisabled(req.biomes.empty() || sections.empty());
    if (ImGui::Button("Generate Vegetation", ImVec2(-1, 0))) {
        editor::BiomeVegetationOutcome out = editor::generateBiomeVegetation(sections, req, contentDir);
        for (const std::string& w : out.warnings) AVER_WARN("[Vegetation] {}", w);
        if (!out.ok) {
            u.status = "Generate vegetation: " + out.error;
            setUpgradeStatus(u.status, editor::NotifySeverity::Warning);
        } else {
#  if AVER_MODULE_VOXI
            if (rset_.voxiAttached_) levelFoliage_.setExtraTable(fnv1a64(std::string_view(req.ocinstRel)), std::move(out.table), std::move(out.groupMaterials));
#  endif
            if (std::find(lvl_.levelHeader_.foliageFiles.begin(), lvl_.levelHeader_.foliageFiles.end(), req.ocinstRel) == lvl_.levelHeader_.foliageFiles.end()) {
                lvl_.levelHeader_.foliageFiles.push_back(req.ocinstRel);
                markLevelUnsaved();
            }
            u.status = editor::biomes::outcomeLine(out.instances, out.kept, out.cellsCapped);
            setUpgradeStatus(u.status);
        }
    }
    ImGui::EndDisabled();
    uiReg_.track("landscape.biomes.generate");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(sections.empty() ? "Load a landscape first."
                          : req.biomes.empty() ? "Tick at least one biome."
                          : "Runs the ticked biomes over every landscape section (priority decides overlaps) into\n"
                            "Content/Foliage/<name>_vegetation.ocinst with a .ocveg record. Deterministic; running it\n"
                            "again regenerates the table and keeps manual edits.");
    if (!u.status.empty()) ImGui::TextWrapped("%s", u.status.c_str());
}

}  // namespace aver

#endif
