// Editor: the Landscape mode's Paint tab -- terrain material row, layer list, paint / erase / smooth brush, fill, slope
// auto-fill, removing unused layers, layer-driven foliage scatter -- and the layered-terrain section of the material
// panel. Part of SandboxApp (declared in SandboxApp.hpp); the pure helpers are in LandscapePaintTool.hpp.

#include "SandboxApp.hpp"
#include "LandscapePaintTool.hpp"
#include "VegetationGenerate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver {

#if AVER_MODULE_LANDSCAPE
using GameLandscapeLayer = game::GameLandscape::LayerInfo;
#endif

// ---------------------------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------------------------
namespace editor {
namespace {
// A small integer hash (lowbias32 by Chris Wellons), good enough for jitter.
u32 mix32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
u32 hash3(u32 a, u32 b, u32 c) { return mix32(a * 0x9e3779b1u ^ mix32(b + 0x85ebca6bu) ^ mix32(c * 0xc2b2ae35u + 0x27d4eb2fu)); }
f32 unit(u32 h) { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }
} // namespace

void layerScatterCandidates(const fmt::OcLandData& d, u32 sectionLayer, f32 densityPer100m2, f32 minWeight, u32 salt,
                            usize cap, std::vector<ScatterPoint>& out) {
    out.clear();
    if (!d.valid() || !d.layersValid() || sectionLayer >= d.layerNames.size() || !(densityPer100m2 > 0.0f)) return;
    const f32 cell = std::sqrt(1.0e6f / densityPer100m2);   // cell area = 1 / density, in cm^2
    const f32 extent = d.extentCm();
    const u32 cells = static_cast<u32>(std::ceil(extent / cell));
    for (u32 iy = 0; iy < cells; ++iy) {
        for (u32 ix = 0; ix < cells; ++ix) {
            const u32 h = hash3(salt ^ (sectionLayer * 0x1234567u), ix, iy);
            const f32 x = d.originCm[0] + (static_cast<f32>(ix) + unit(mix32(h ^ 0xa511e9b3u))) * cell;
            const f32 y = d.originCm[1] + (static_cast<f32>(iy) + unit(mix32(h ^ 0x63d83595u))) * cell;
            if (x > d.originCm[0] + extent || y > d.originCm[1] + extent) continue;
            f32 w[8];
            if (!landscape::layerWeightsAt(d, x, y, w)) continue;
            if (w[sectionLayer] < minWeight) continue;
            out.push_back({x, y, mix32(h ^ 0x1b873593u) | 1u});
            if (out.size() >= cap) return;
        }
    }
}
} // namespace editor

#if AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI

// ---------------------------------------------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------------------------------------------

// Ends a weight stroke and pushes ONE undo entry for it (none when nothing changed).
void SandboxApp::endPaintStroke(const char* label) {
    painting_ = false;
    if (!landscape_.weightStrokeActive()) return;
    EditCmd c;
    c.kind = EditCmd::Kind::LandscapeWeights;
    c.label = label ? label : "Paint terrain";
    if (!landscape_.endWeightStroke(c.landWeights)) return;   // one entry for every section the stroke touched
    pushEdit(std::move(c));
}

// Removing a layer changes how many planes a recorded rect has, so weight entries can no longer be replayed.
void SandboxApp::purgeWeightUndo() {
    const auto weights = [](const EditCmd& c) { return c.kind == EditCmd::Kind::LandscapeWeights; };
    std::erase_if(undoStack_, weights);
    std::erase_if(redoStack_, weights);
}

// The viewport half: LMB paints the selected layer under the cursor while held.
void SandboxApp::handlePaint(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
    sculptCursorValid_ = false;
    if (!landscape_.loaded() || !landscape_.terrainMaterialActive() || playSessionActive()) {
        painting_ = false;
        if (landscape_.weightStrokeActive()) endPaintStroke();
        return;
    }

    Vec3 ro, rd;
    viewportRay(mx, my, ro, rd);
    const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
    landscape::HeightfieldHit hit;
    const bool haveHit = overScene && landscape_.raycast(roA, rdA, hit);
    if (haveHit) {
        sculptCursorValid_ = true;   // the same brush ring the sculpt tool draws
        sculptCursor_ = Vec3{hit.posCm[0], hit.posCm[1], hit.posCm[2]};
    }

    if (ImGui::IsMouseClicked(0) && overScene && haveHit) {
        // Each section the brush reaches gets the layer added on the spot, before its share of the stroke is captured.
        landscape_.beginWeightStroke();
        painting_ = true;
    }
    if (!io.MouseDown[0] && landscape_.weightStrokeActive()) endPaintStroke();

    if (painting_ && haveHit) {
        landscape::PaintParams p;
        p.centerCm[0] = hit.posCm[0];
        p.centerCm[1] = hit.posCm[1];
        p.radiusCm = paintRadiusCm_;
        p.strength = paintStrength_;
        p.falloff = paintFalloff_;
        p.target = paintOpacity_;
        p.mode = paintMode_ == 1 ? landscape::PaintMode::Erase : paintMode_ == 2 ? landscape::PaintMode::Smooth
                                                                                 : landscape::PaintMode::Paint;
        // dt-scaled like the sculpt brush, so holding paints at a rate that does not depend on the frame rate.
        const f32 dt = std::fmin(e.time().dt, 0.05f);
        landscape_.paint(e.device(), p, std::fmin(1.0f, dt * 6.0f), landscape_.materialLayerName(static_cast<u32>(paintLayer_)));
    }
    if (!io.MouseDown[0]) painting_ = false;
}

void SandboxApp::buildLandscapePaintTab(Engine& e) {
#if AVER_MODULE_PBR
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    if (levelHeader_.landscapes.empty()) recordLandscapeInLevel();
    const std::string current = levelHeader_.landscapes.empty() ? std::string() : levelHeader_.landscapes.front().material;

    ImGui::TextDisabled("TERRAIN MATERIAL");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##terrainMaterial", current.empty() ? "(none)" : current.c_str())) {
        for (u32 i = 0; i < lib.count(); ++i) {
            const pbr::MaterialDesc* d = lib.desc(lib.at(i));
            if (!d || !d->terrain.enabled() || d->name.find('#') != std::string::npos) continue;   // '#': a derived copy
            if (ImGui::Selectable(d->name.c_str(), d->name == current) && !levelHeader_.landscapes.empty() &&
                !landscape_.weightStrokeActive()) {
                levelHeader_.landscapes.front().material = d->name;
                landscape_.setSurfaceMaterial(d->name);
                markLevelUnsaved();
            }
        }
        ImGui::EndCombo();
    }
    uiReg_.track("landscape.terrainMaterial");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Materials with a TERRAIN block (Details > Material > Terrain layers).\n"
                          "Paint is stored in the .ocland by layer name, so reordering layers is safe.");

    if (!landscape_.terrainMaterialActive()) {
        ImGui::Spacing();
        ImGui::TextWrapped("The landscape's material has no terrain layers. Pick a layered terrain material above, or open a "
                           "material and press \"Make terrain material\" in its Details panel.");
        return;
    }
    const bool locked = playSessionActive();
    if (locked)
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.15f, 1.0f), "Painting is disabled while a Play session runs.");
    ImGui::BeginDisabled(locked);

    // ---- layers ----
    ImGui::Spacing();
    ImGui::TextDisabled("LAYERS");
    const u32 n = landscape_.materialLayerCount();
    if (paintLayer_ < 0 || static_cast<u32>(paintLayer_) >= n) paintLayer_ = 0;
    pbr::TerrainTextures info;
    bool haveInfo = false;
    if (const pbr::MaterialHandle bh = current.empty() ? 0 : content_.materialForSurface(current))
        if (const pbr::MaterialDesc* bd = lib.desc(bh)) haveInfo = voxiRenderer_.materials().terrainTextures(*bd, info);
    for (u32 i = 0; i < n; ++i) {
        const GameLandscapeLayer* l = landscape_.materialLayer(i);
        if (!l) continue;
        ImGui::PushID(static_cast<int>(i));
        const f32 avg[3] = {haveInfo ? info.avg[i][0] : 0.4f, haveInfo ? info.avg[i][1] : 0.4f, haveInfo ? info.avg[i][2] : 0.4f};
        const ImVec4 swatch(std::pow(avg[0], 1.0f / 2.2f), std::pow(avg[1], 1.0f / 2.2f), std::pow(avg[2], 1.0f / 2.2f), 1.0f);
        ImGui::ColorButton("##sw", swatch, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(18.0f, 18.0f));
        ImGui::SameLine();
        if (ImGui::Selectable(l->name.c_str(), paintLayer_ == static_cast<int>(i), 0, ImVec2(-1.0f, 0.0f)))
            paintLayer_ = static_cast<int>(i);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nSurface %s, friction %.2f, restitution %.2f%s", l->name.c_str(), l->surfaceName().c_str(),
                              l->friction, l->restitution, i == 0 ? "\nThe base layer: whatever is not painted." : "");
        ImGui::PopID();
    }

    // ---- tool + brush ----
    ImGui::Spacing();
    ImGui::TextDisabled("TOOL");
    static const char* kPaintModes[3] = {"Paint", "Erase", "Smooth"};
    for (int i = 0; i < 3; ++i) {
        const bool on = paintMode_ == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, kAverOrangeDim);
        if (ImGui::Button(kPaintModes[i], ImVec2(-1, 0))) paintMode_ = i;
        if (on) ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("BRUSH");
    editor::panelFloat("Radius (cm)", &paintRadiusCm_, 50.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelFloat("Strength", &paintStrength_, 0.02f, 1.0f, "%.2f");
    editor::panelFloat("Falloff", &paintFalloff_, 0.0f, 1.0f, "%.2f");
    editor::panelFloat("Opacity", &paintOpacity_, 0.05f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint stops at this weight, so a layer can be laid down thin.");

    // ---- whole-section commands ----
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("COMMANDS");
    const std::string layerName = landscape_.materialLayerName(static_cast<u32>(paintLayer_));
    if (ImGui::Button("Fill Layer", ImVec2(-1, 0))) {
        landscape_.beginWeightStroke();
        landscape_.fillLayerAll(layerName);
        endPaintStroke("Fill terrain layer");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paints every section with the selected layer. One undo step.");
    if (ImGui::Button("Auto-fill By Slope / Height...", ImVec2(-1, 0))) ImGui::OpenPopup("##autoFill");
    if (ImGui::BeginPopup("##autoFill")) {
        ImGui::TextUnformatted("Paint the selected layer where...");
        ImGui::SliderFloat("Min slope (deg)", &autoSlopeMinDeg_, 0.0f, 90.0f, "%.0f");
        ImGui::SliderFloat("Max slope (deg)", &autoSlopeMaxDeg_, 0.0f, 90.0f, "%.0f");
        ImGui::SliderFloat("Feather (deg)", &autoFeatherDeg_, 0.0f, 30.0f, "%.0f");
        ImGui::DragFloat("Min height (cm)", &autoZMinCm_, 10.0f);
        ImGui::DragFloat("Max height (cm)", &autoZMaxCm_, 10.0f);
        if (ImGui::Button("Apply")) {
            landscape_.beginWeightStroke();
            landscape_.autoFillBySlopeAll(layerName, autoSlopeMinDeg_, autoSlopeMaxDeg_, autoZMinCm_, autoZMaxCm_, autoFeatherDeg_, 1.0f);
            endPaintStroke("Auto-fill terrain layer");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::Button("Remove Unused Layers", ImVec2(-1, 0))) {
        u32 removed = 0;
        for (u32 sec = 0; sec < landscape_.sectionCount(); ++sec) {
            const fmt::OcLandData& d = landscape_.sectionData(sec);
            const usize nn = static_cast<usize>(d.sampleCount) * d.sampleCount;
            for (u32 s = static_cast<u32>(d.layerNames.size()); s-- > 1;) {
                bool used = false;
                for (usize i = 0; i < nn && !used; ++i) used = d.weights[(s - 1) * nn + i] != 0;
                if (!used && landscape_.removeLayer(sec, s)) ++removed;
            }
        }
        if (removed) { purgeWeightUndo(); setUpgradeStatus("Removed " + std::to_string(removed) + " unused terrain layer(s)"); }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drops section layers with no paint. Clears the paint undo history (the layer table changes).");

#if AVER_MODULE_SCENE
    const GameLandscapeLayer* sel = landscape_.materialLayer(static_cast<u32>(paintLayer_));
    ImGui::BeginDisabled(!sel || sel->foliage.empty() || sel->foliageDensity <= 0.0f);
    if (ImGui::Button("Scatter Foliage From Layer", ImVec2(-1, 0))) scatterFoliageFromLayer(e);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Generates the layer's foliage type (set on the layer in its material) at the layer's density where its\n"
                          "paint weight is high enough, into Content/Foliage/<layer>_vegetation.ocinst with a .ocveg record.\n"
                          "Deterministic; running it again regenerates the table and keeps manual edits.");
    buildBiomeSection();   // LandscapeBiomeTab.cpp: the biome list and Generate Vegetation
#endif
    ImGui::EndDisabled();
#else
    (void)e;
    ImGui::TextWrapped("Terrain layers need the PBR module.");
#endif
}

#if AVER_MODULE_SCENE
// Layer foliage: a thin call into Generate Vegetation (docs/PCG.md, W4). The layer's foliage type, density and minimum weight make a
// one-entry biome that runs over every landscape section; the result is a cell-sorted .ocinst plus a .ocveg record under
// Content/Foliage, shown through the level's extra foliage tables right away and listed in the level's FOLIAGE files so a save keeps
// it. A second run regenerates it (same seed, same table) and keeps hand edits recorded in the .ocveg.
void SandboxApp::scatterFoliageFromLayer(Engine& e) {
    (void)e;
    const GameLandscapeLayer* l = landscape_.materialLayer(static_cast<u32>(paintLayer_));
    if (!l || l->foliage.empty() || l->foliageDensity <= 0.0f || !project_.valid()) return;

    std::string rel = l->foliage;   // content-relative; an absolute path is made relative when it lies under Content
    for (char& c : rel) if (c == '\\') c = '/';
    const std::string contentDir = project_.contentDir();
    std::string contentFwd = contentDir;
    for (char& c : contentFwd) if (c == '\\') c = '/';
    if (rel.size() > contentFwd.size() && rel.compare(0, contentFwd.size(), contentFwd) == 0) rel = rel.substr(contentFwd.size() + 1);

    editor::LayerVegetationRequest req;
    req.layer = l->name;
    req.foliageRel = rel;
    req.densityPer100m2 = l->foliageDensity;
    req.minWeight = l->foliageMinWeight;
    req.seed = static_cast<i32>(fnv1a64(std::string_view(l->name)) & 0x7fffffff);
    req.ocinstRel = "Foliage/" + l->name + "_vegetation.ocinst";
    std::string why;
    if (!fmt::loadOcFoliage(contentDir + "/" + rel, req.type, &why)) {
        setUpgradeStatus("Layer foliage '" + l->foliage + "' cannot be read: " + why, editor::NotifySeverity::Warning);
        return;
    }
    std::vector<const fmt::OcLandData*> sections;
    for (u32 i = 0; i < landscape_.sectionCount(); ++i) sections.push_back(&landscape_.sectionData(i));
    editor::LayerVegetationOutcome out = editor::generateLayerVegetation(sections, req, contentDir);
    for (const std::string& w : out.warnings) AVER_WARN("[Vegetation] {}", w);
    if (!out.ok) { setUpgradeStatus("Generate vegetation: " + out.error, editor::NotifySeverity::Warning); return; }

#  if AVER_MODULE_VOXI
    if (voxiAttached_) {
        std::vector<std::string> mats(out.table.groups.size(), req.type.material);
        levelFoliage_.setExtraTable(fnv1a64(std::string_view(req.ocinstRel)), std::move(out.table), std::move(mats));
    }
#  endif
    if (std::find(levelHeader_.foliageFiles.begin(), levelHeader_.foliageFiles.end(), req.ocinstRel) == levelHeader_.foliageFiles.end())
        levelHeader_.foliageFiles.push_back(req.ocinstRel);
    setUpgradeStatus("Generated " + std::to_string(out.instances) + " instance(s) of " + std::filesystem::path(rel).stem().string() +
                     " on " + l->name + (out.kept ? " (kept " + std::to_string(out.kept) + " manual edit(s))" : std::string()));
}
#endif // AVER_MODULE_SCENE

#if AVER_MODULE_PBR
// The "Terrain layers" section of the material panel. `track` is materialPanel's undo bracket (it records whether the last
// widget started / finished an interaction and passes its change flag through). True when anything changed.
bool SandboxApp::buildTerrainLayersSection(pbr::MaterialDesc& d, const std::function<bool(bool)>& track) {
    bool changed = false;
    pbr::TerrainDesc& t = d.terrain;
    ImGui::Separator();
    ImGui::TextDisabled("TERRAIN LAYERS");
    if (!t.enabled()) {
        if (ImGui::Button("Make terrain material")) {
            pbr::TerrainLayerDesc base;
            base.name = "Base";
            t.layers.push_back(base);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Turns this into a layered terrain material: up to 8 named layers blended by the landscape's paint.\n"
                              "The texture slots below are then unused (the layers carry their own).");
        return changed;
    }

    static const int kTiles[3] = {256, 512, 1024}, kPads[3] = {4, 8, 16};
    int ti = t.tileTexels <= 256 ? 0 : t.tileTexels <= 512 ? 1 : 2;
    if (ImGui::Combo("Tile size (texels)", &ti, "256\0" "512\0" "1024\0")) { t.tileTexels = static_cast<u32>(kTiles[ti]); changed = true; }
    track(false);
    if (t.tileTexels >= 1024 && ImGui::IsItemHovered()) ImGui::SetTooltip("1024 texel tiles take ~140 MB of GPU memory per material.");
    int pi = t.padTexels <= 4 ? 0 : t.padTexels <= 8 ? 1 : 2;
    if (ImGui::Combo("Edge pad (texels)", &pi, "4\0" "8\0" "16\0")) { t.padTexels = static_cast<u32>(kPads[pi]); changed = true; }
    track(false);
    changed |= track(ImGui::SliderFloat("Height blend", &t.blendDepth, 0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far a layer's height map lets it win a mix: small = crisp, rocks poking through grass;\n"
                          "large (above 0.5 fades to plain linear weights) = soft.");

    int remove = -1, moveUp = -1;
    for (u32 i = 0; i < t.layers.size(); ++i) {
        pbr::TerrainLayerDesc& l = t.layers[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string title = std::to_string(i) + "  " + l.name + (i == 0 ? "  (base)" : "");
        if (::aver::editor::sectionHeader(title.c_str())) {
            char nameBuf[40];
            std::snprintf(nameBuf, sizeof nameBuf, "%s", l.name.c_str());
            if (ImGui::InputText("Name", nameBuf, sizeof nameBuf)) {
                // [A-Za-z0-9_], unique: paint refers to layers by this name.
                std::string nm;
                for (const char* c = nameBuf; *c; ++c)
                    if (std::isalnum(static_cast<unsigned char>(*c)) || *c == '_') nm += *c;
                bool dup = nm.empty();
                for (u32 k = 0; k < t.layers.size(); ++k) if (k != i && t.layers[k].name == nm) dup = true;
                if (!dup) { l.name = nm; changed = true; }
            }
            track(false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint is stored by this name: renaming a painted layer detaches its paint.");
            const auto texRef = [&](const char* label, pbr::TextureRef& r) {
                char buf[260];
                std::snprintf(buf, sizeof buf, "%s", r.path.c_str());
                if (ImGui::InputText(label, buf, sizeof buf)) { r.path = buf; r.id = 0; changed = true; }
                track(false);
            };
            texRef("Base colour", l.baseColor);
            texRef("Normal", l.normal);
            texRef("Metal / rough (ORM)", l.metalRough);
            texRef("Height", l.height);
            changed |= track(ImGui::DragFloat("Tiling (cm per tile)", &l.tilingCm, 5.0f, 5.0f, 100000.0f, "%.0f"));
            changed |= track(ImGui::SliderFloat("Height bias", &l.heightBias, -1.0f, 1.0f, "%.2f"));
            char surf[64];
            std::snprintf(surf, sizeof surf, "%s", l.surface.c_str());
            if (ImGui::InputText("Surface", surf, sizeof surf)) { l.surface = surf; changed = true; }
            track(false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Footstep / physical surface name (empty = the layer's name).");
            changed |= track(ImGui::SliderFloat("Friction", &l.friction, 0.0f, 2.0f, "%.2f"));
            changed |= track(ImGui::SliderFloat("Restitution", &l.restitution, 0.0f, 1.0f, "%.2f"));
            char fol[260];
            std::snprintf(fol, sizeof fol, "%s", l.foliage.c_str());
            if (ImGui::InputText("Foliage (.ocfoliage)", fol, sizeof fol)) { l.foliage = fol; changed = true; }
            track(false);
            changed |= track(ImGui::DragFloat("Foliage per 100 m^2", &l.foliageDensity, 0.5f, 0.0f, 5000.0f, "%.1f"));
            changed |= track(ImGui::SliderFloat("Foliage min weight", &l.foliageMinWeight, 0.0f, 1.0f, "%.2f"));
            if (ImGui::SmallButton("Move up") && i > 0) moveUp = static_cast<int>(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove") && t.layers.size() > 1) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (moveUp > 0) { std::swap(t.layers[static_cast<usize>(moveUp)], t.layers[static_cast<usize>(moveUp) - 1]); changed = true; }
    if (remove >= 0) { t.layers.erase(t.layers.begin() + remove); changed = true; }
    ImGui::BeginDisabled(t.layers.size() >= pbr::kMaxTerrainLayers);
    if (ImGui::Button("Add layer")) {
        pbr::TerrainLayerDesc nl;
        for (u32 k = 1;; ++k) {
            nl.name = "Layer" + std::to_string(k);
            bool dup = false;
            for (const pbr::TerrainLayerDesc& o : t.layers) dup |= o.name == nl.name;
            if (!dup) break;
        }
        t.layers.push_back(nl);
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}
#endif // AVER_MODULE_PBR

#endif // AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI

} // namespace aver
