// The Groom tab and the Hair Details section. See HairGroomTab.hpp.
#include "HairGroomTab.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>

#if AVER_MODULE_HAIR
#  include "aver/formats/GltfImport.hpp"
#  include "aver/hair/HairCurves.hpp"
#endif

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "EditorKeybinds.hpp"
#  include "imgui.h"
#endif

#if AVER_WITH_IMGUI && AVER_MODULE_HAIR_SCENE
#  include "aver/hair/HairSystem.hpp"
#  include "aver/scene/World.hpp"
#endif

namespace aver::editor {

namespace {

std::string joinJoints(const std::vector<std::string>& joints) {
    std::string out;
    for (usize i = 0; i < joints.size(); ++i) {
        if (i) out += ", ";
        out += joints[i];
    }
    return out;
}

// "head, neck_01 spine_03" -> {"head", "neck_01", "spine_03"}: commas and white space both separate, empty names are dropped.
std::vector<std::string> splitJoints(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    if (out.size() > fmt::kOcHairMaxScalpJoints) out.resize(fmt::kOcHairMaxScalpJoints);
    return out;
}

void copyText(char* dst, usize cap, const std::string& s) { std::snprintf(dst, cap, "%s", s.c_str()); }

} // namespace

// Keeps the list selections inside the lists after an edit, an undo or a redo.
void HairGroomEditor::clampSelection() {
    selMaterial_ = std::clamp<int>(selMaterial_, 0, std::max<int>(static_cast<int>(state_.materials.size()) - 1, 0));
    selLook_ = std::clamp<int>(selLook_, 0, std::max<int>(static_cast<int>(state_.looks.size()) - 1, 0));
    selProxy_ = std::clamp<int>(selProxy_, 0, std::max<int>(static_cast<int>(state_.proxies.size()) - 1, 0));
}

// ================================================================================ the Groom tab: state

HairGroomEditor::HairGroomEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void HairGroomEditor::loadFromDisk() {
    std::string why;
    fmt::OcHairData loaded;
    if (!fmt::loadOcHair(path_, loaded, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    // The strands and the embedded scalp sit beside the records so the undo snapshots stay small.
    strands_ = std::move(loaded.strands);
    loaded.strands = fmt::OcHairStrands{};
    hasScalp_ = loaded.hasScalp;
    scalp_ = std::move(loaded.scalp);
    loaded.scalp = fmt::OcHairScalp{};
    loaded.hasScalp = false;
    state_ = std::move(loaded);
    hairmodel::sanitize(state_);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    gestureOpen_ = false;
    history_.clear();
    previewStale_ = true;
    previewScalpKey_.clear();
    adviceStale_ = true;
    selMaterial_ = selLook_ = selProxy_ = 0;
    syncBuffers();
}

void HairGroomEditor::syncBuffers() {
    copyText(scalpBuf_, sizeof scalpBuf_, state_.scalpMesh);
    copyText(jointsBuf_, sizeof jointsBuf_, joinJoints(state_.scalpJoints));
    copyText(maskBuf_, sizeof maskBuf_, state_.generate.mask);
}

std::string HairGroomEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor whose dirty() is true (AssetEditor.cpp).
    return std::filesystem::path(path_).filename().string() + "###hairgroom:" + path_;
}

fmt::OcHairData HairGroomEditor::assembled() const {
    fmt::OcHairData g = state_;
    g.strands = strands_;
    g.hasScalp = hasScalp_;
    if (hasScalp_) g.scalp = scalp_;
    return g;
}

bool HairGroomEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!fmt::saveOcHair(path_, assembled(), why)) return false;
    dirty_ = false;
    return true;
}

void HairGroomEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS (FoliageTypeEditor's rule): reloading would discard what the author typed because something else touched the file.
    if (dirty_) {
        AVER_WARN("[HairGroomEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

// ================================================================================ undo and edits

void HairGroomEditor::pushUndo() { history_.push(state_); }

void HairGroomEditor::undo() {
    if (!history_.undo(state_)) return;
    dirty_ = true;
    previewStale_ = true;
    adviceStale_ = true;
    previewScalpKey_.clear();
    clampSelection();
    syncBuffers();
}

void HairGroomEditor::redo() {
    if (!history_.redo(state_)) return;
    dirty_ = true;
    previewStale_ = true;
    adviceStale_ = true;
    previewScalpKey_.clear();
    clampSelection();
    syncBuffers();
}

void HairGroomEditor::editInGesture(const std::function<void(fmt::OcHairData&)>& mutate) {
    if (!loaded_) return;
    mutate(state_);
    hairmodel::sanitize(state_);
    clampSelection();
    dirty_ = true;
    previewStale_ = true;
    adviceStale_ = true;
}

void HairGroomEditor::edit(const std::function<void(fmt::OcHairData&)>& mutate) {
    if (!loaded_) return;
    pushUndo();
    editInGesture(mutate);
}

namespace {

// The smallest index in 0..255 that no record of the list uses, or 256 when all are taken.
template <typename List, typename IndexOf>
u32 firstFreeIndex(const List& list, IndexOf indexOf) {
    std::set<u32> used;
    for (const auto& r : list) used.insert(indexOf(r));
    for (u32 i = 0; i <= fmt::kOcHairMaxMaterials; ++i)
        if (!used.count(i)) return i;
    return fmt::kOcHairMaxMaterials + 1;
}

} // namespace

bool HairGroomEditor::addMaterial() {
    if (!loaded_) return false;
    const u32 idx = firstFreeIndex(state_.materials, [](const fmt::OcHairMaterial& m) { return m.index; });
    if (idx > fmt::kOcHairMaxMaterials) return false;
    edit([idx](fmt::OcHairData& g) {
        fmt::OcHairMaterial m;
        m.index = idx;
        g.materials.push_back(m);
    });
    selMaterial_ = static_cast<int>(state_.materials.size()) - 1;
    return true;
}

bool HairGroomEditor::removeMaterial(u32 index) {
    if (!loaded_ || state_.materials.size() <= 1) return false;
    const bool has = std::any_of(state_.materials.begin(), state_.materials.end(), [index](const fmt::OcHairMaterial& m) { return m.index == index; });
    if (!has) return false;
    edit([index](fmt::OcHairData& g) {
        g.materials.erase(std::remove_if(g.materials.begin(), g.materials.end(), [index](const fmt::OcHairMaterial& m) { return m.index == index; }), g.materials.end());
    });
    return true;
}

bool HairGroomEditor::addLook() {
    if (!loaded_) return false;
    const u32 idx = firstFreeIndex(state_.looks, [](const fmt::OcHairLook& l) { return l.material; });
    if (idx > fmt::kOcHairMaxMaterials) return false;
    edit([idx](fmt::OcHairData& g) {
        fmt::OcHairLook l;
        l.material = idx;
        g.looks.push_back(l);
    });
    selLook_ = static_cast<int>(state_.looks.size()) - 1;
    return true;
}

bool HairGroomEditor::removeLook(u32 index) {
    if (!loaded_ || state_.looks.size() <= 1) return false;
    const bool has = std::any_of(state_.looks.begin(), state_.looks.end(), [index](const fmt::OcHairLook& l) { return l.material == index; });
    if (!has) return false;
    edit([index](fmt::OcHairData& g) {
        g.looks.erase(std::remove_if(g.looks.begin(), g.looks.end(), [index](const fmt::OcHairLook& l) { return l.material == index; }), g.looks.end());
    });
    return true;
}

bool HairGroomEditor::addProxy(const fmt::OcHairProxy& p) {
    if (!loaded_ || state_.proxies.size() >= 64) return false;
    edit([p](fmt::OcHairData& g) { g.proxies.push_back(p); });
    selProxy_ = static_cast<int>(state_.proxies.size()) - 1;
    return true;
}

bool HairGroomEditor::removeProxy(usize i) {
    if (!loaded_ || i >= state_.proxies.size()) return false;
    edit([i](fmt::OcHairData& g) { g.proxies.erase(g.proxies.begin() + static_cast<std::ptrdiff_t>(i)); });
    return true;
}

bool HairGroomEditor::useStarterProxies() {
#if AVER_MODULE_HAIR
    if (!loaded_) return false;
    edit([](fmt::OcHairData& g) { g.proxies = hairmodel::starterProxies(); });
    selProxy_ = 0;
    return true;
#else
    return false;
#endif
}

void HairGroomEditor::clearStoredStrands() {
    if (!loaded_ || strands_.empty()) return;
    strands_ = fmt::OcHairStrands{};
    if (!state_.generate.present) {
        fmt::OcHairData fresh = hairmodel::starterGroom();
        state_.generate = fresh.generate;
    }
    hairmodel::sanitize(state_);
    dirty_ = true;
    previewStale_ = true;
    adviceStale_ = true;
}

bool HairGroomEditor::importCurves(const std::string& gltfPath, std::string* why) {
#if AVER_MODULE_HAIR
    if (!loaded_) {
        if (why) *why = "the groom is not loaded";
        return false;
    }
    fmt::GltfImportResult res;
    if (!fmt::importGltf(gltfPath, res, fmt::GltfImportOptions{}, why)) return false;
    hair::CurveImportOptions opt;
    hair::CurveImportReport rep;
    fmt::OcHairStrands made;
    if (!hair::curvesToStrands(res.hairCurves, made, opt, &rep, why)) return false;
    if (made.strandCount() > state_.maxRenderStrands) {
        if (why) *why = "the file has " + std::to_string(made.strandCount()) + " strands and MAXSTRANDS is " + std::to_string(state_.maxRenderStrands);
        return false;
    }
    strands_ = std::move(made);
    // A MATERIAL and a LOOK for every strand material the curves use, as AverAssetC hair writes them.
    std::set<u32> used(strands_.material.begin(), strands_.material.end());
    for (u32 m : used) {
        if (!state_.findMaterial(m)) { fmt::OcHairMaterial rec; rec.index = m; state_.materials.push_back(rec); }
        if (!state_.findLook(m)) { fmt::OcHairLook rec; rec.material = m; state_.looks.push_back(rec); }
    }
    hairmodel::sanitize(state_);
    dirty_ = true;
    previewStale_ = true;
    adviceStale_ = true;
    return true;
#else
    (void)gltfPath;
    if (why) *why = "this build has no Aver.Hair (AVER_MODULE_HAIR is off)";
    return false;
#endif
}

// ================================================================================ preview and advice

void HairGroomEditor::rebuildPreview() {
    previewStale_ = false;
    previewNextMs_ = 0.0;
#if AVER_MODULE_HAIR
    const std::string key = (hasScalp_ ? std::string("<embedded>") : state_.scalpMesh) + "#" + std::to_string(state_.scalpSubmesh);
    if (key != previewScalpKey_) {
        previewScalpKey_ = key;
        previewScalpError_.clear();
        previewHasScalp_ = hairmodel::loadPreviewScalp(state_, hasScalp_ ? &scalp_ : nullptr, path_, previewScalp_, &previewScalpError_);
        adviceStale_ = true;
    }
    if (!previewHasScalp_) {
        preview_ = hairmodel::GroomPreview{};
        preview_.error = previewScalpError_;
        return;
    }
    hairmodel::buildPreview(state_, strands_, previewScalp_, 300, preview_);
#else
    preview_ = hairmodel::GroomPreview{};
    preview_.error = "this build has no Aver.Hair";
#endif
}

const hairmodel::GroomPreview& HairGroomEditor::preview() {
    if (previewStale_) rebuildPreview();
    return preview_;
}

const std::vector<std::string>& HairGroomEditor::advice() {
    // Uses the scalp state of the last preview build; the preview is rebuilt by preview() or, in the window, by draw()'s throttle.
    if (adviceStale_) {
        adviceStale_ = false;
        advice_ = hairmodel::groomAdvice(assembled(), budget(), previewHasScalp_);
    }
    return advice_;
}

// ================================================================================ drawing

#if AVER_WITH_IMGUI

HairGroomEditor::ItemState HairGroomEditor::probe() {
    ItemState s;
    s.activated = ImGui::IsItemActivated();
    s.active = ImGui::IsItemActive();
    s.hovered = ImGui::IsItemHovered();
    return s;
}

void HairGroomEditor::field(bool changed, const ItemState& item, const std::function<void(fmt::OcHairData&)>& mutate) {
    if (!changed) return;
    // The first change of a drag (or any discrete change) opens an undo step; the rest of the drag edits inside it.
    if (item.activated || !gestureOpen_) edit(mutate);
    else editInGesture(mutate);
    gestureOpen_ = item.active;
}

void HairGroomEditor::dragFloat(const char* label, f32 value, f32 speed, f32 lo, f32 hi, const char* format, const std::function<void(fmt::OcHairData&, f32)>& set, const char* tip) {
    f32 v = value;
    const bool changed = ImGui::DragFloat(label, &v, speed, lo, hi, format);
    const ItemState item = probe();
    if (tip && item.hovered) ImGui::SetTooltip("%s", tip);
    field(changed, item, [set, v](fmt::OcHairData& g) { set(g, v); });
}

void HairGroomEditor::dragInt(const char* label, i32 value, f32 speed, i32 lo, i32 hi, const std::function<void(fmt::OcHairData&, i32)>& set, const char* tip) {
    int v = value;
    const bool changed = ImGui::DragInt(label, &v, speed, lo, hi);
    const ItemState item = probe();
    if (tip && item.hovered) ImGui::SetTooltip("%s", tip);
    field(changed, item, [set, v](fmt::OcHairData& g) { set(g, static_cast<i32>(v)); });
}

void HairGroomEditor::checkbox(const char* label, bool value, const std::function<void(fmt::OcHairData&, bool)>& set, const char* tip) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    const ItemState item = probe();
    if (tip && item.hovered) ImGui::SetTooltip("%s", tip);
    field(changed, item, [set, v](fmt::OcHairData& g) { set(g, v); });
}

void HairGroomEditor::gradientRow(const char* label, const fmt::OcHairGradient& g, const std::function<void(fmt::OcHairData&, const fmt::OcHairGradient&)>& set, const char* tip) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    float vals[2] = {g.min, g.max};
    ImGui::SetNextItemWidth(160.0f);
    bool changed = ImGui::DragFloat2("##values", vals, 0.01f, 0.0f, 0.0f, "%.4g");
    ItemState item = probe();
    if (item.hovered) ImGui::SetTooltip("Value at the two ends of the gradient: min, max");
    field(changed, item, [set, g, vals](fmt::OcHairData& d) {
        fmt::OcHairGradient n = g;
        n.min = vals[0];
        n.max = vals[1];
        set(d, n);
    });
    ImGui::SameLine();
    float fr[2] = {g.minFraction, g.maxFraction};
    ImGui::SetNextItemWidth(160.0f);
    changed = ImGui::DragFloat2("##fractions", fr, 0.005f, 0.0f, 1.0f, "%.2f");
    item = probe();
    if (item.hovered) ImGui::SetTooltip("Where along the strand (0 root, 1 tip) the value is min and where it is max. The two cannot be equal.");
    field(changed, item, [set, g, fr](fmt::OcHairData& d) {
        fmt::OcHairGradient n = g;
        n.minFraction = fr[0];
        n.maxFraction = fr[1];
        set(d, n);
    });
    ImGui::PopID();
}

void HairGroomEditor::drawToolbar() {
    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[HairGroomEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && editor::keybinds().pressed(editor::CommandId::EditUndo, io)) undo();
            if (canRedo() && editor::keybinds().pressed(editor::CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh preview")) { previewStale_ = true; previewScalpKey_.clear(); adviceStale_ = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reads the scalp mesh again and regrows the preview (after the mesh changed on disk).");
}

void HairGroomEditor::drawAdvice() {
    const hairmodel::GroomBudget b = budget();
    ImGui::TextDisabled("%llu render strands (%llu simulated), %llu points; draw buffers about %.1f MB",
                        static_cast<unsigned long long>(b.renderStrands), static_cast<unsigned long long>(b.simStrands),
                        static_cast<unsigned long long>(b.renderVertices), static_cast<double>(b.drawBytes) / (1024.0 * 1024.0));
    for (const std::string& line : advice()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", line.c_str());
}

void HairGroomEditor::drawPreview() {
    if (!ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) return;
    static const char* const names[3] = {"Front", "Side", "Top"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        ImGui::RadioButton(names[i], &view_, i);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", hairmodel::previewViewName(static_cast<hairmodel::PreviewView>(view_)));
    const hairmodel::GroomPreview& pv = preview_;
    if (!pv.ok && pv.scalpEdges.empty()) {
        ImGui::TextWrapped("No preview: %s", pv.error.empty() ? "the scalp has not been read yet" : pv.error.c_str());
        return;
    }
    if (ImGui::BeginChild("##hairpreview", ImVec2(0.0f, 300.0f), ImGuiChildFlags_Borders)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const hairmodel::PreviewView view = static_cast<hairmodel::PreviewView>(view_);
        const hairmodel::PreviewFit fit = hairmodel::fitPreview(view, pv.boundsMin, pv.boundsMax, size.x, size.y);
        auto px = [&](const f32* p) {
            f32 u, v, x, y;
            hairmodel::projectPoint(view, p, u, v);
            hairmodel::toPixels(fit, u, v, size.x, size.y, x, y);
            return ImVec2(origin.x + x, origin.y + y);
        };
        for (usize i = 0; i + 6 <= pv.scalpEdges.size(); i += 6)
            dl->AddLine(px(&pv.scalpEdges[i]), px(&pv.scalpEdges[i + 3]), IM_COL32(110, 130, 160, 90), 1.0f);
        for (usize i = 0; i + 6 <= pv.segments.size(); i += 6)
            dl->AddLine(px(&pv.segments[i]), px(&pv.segments[i + 3]), IM_COL32(230, 170, 90, 170), 1.0f);
        for (usize i = 0; i + 3 <= pv.roots.size(); i += 3)
            dl->AddCircleFilled(px(&pv.roots[i]), 1.8f, IM_COL32(120, 230, 150, 230));
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%u of %u strands drawn%s. Roots green, strands amber, scalp grey. A generated groom is previewed with at most 300 strands.", pv.strandsShown,
                        pv.strandsInGroom, pv.spacingCm > 0.0f ? "" : " (root spacing was relaxed or unset)");
}

void HairGroomEditor::drawScalp() {
    if (!ImGui::CollapsingHeader("Scalp", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (hasScalp_) ImGui::TextDisabled("This groom embeds its scalp (SCLP); the mesh path below is ignored.");
    bool changed = ImGui::InputText("Scalp mesh (.ocmesh)", scalpBuf_, sizeof scalpBuf_);
    ItemState item = probe();
    if (item.hovered) ImGui::SetTooltip("Relative to the project's Content folder. The roots grow on these triangles; a skinned mesh brings the joint weights.");
    field(changed, item, [s = std::string(scalpBuf_)](fmt::OcHairData& g) { g.scalpMesh = s; });
    dragInt("Submesh", state_.scalpSubmesh, 0.1f, -1, 64, [](fmt::OcHairData& g, i32 v) { g.scalpSubmesh = v; }, "-1 takes every submesh of the mesh.");
    changed = ImGui::InputText("Scalp joints", jointsBuf_, sizeof jointsBuf_);
    item = probe();
    if (item.hovered) ImGui::SetTooltip("Joint names, separated by commas or spaces (at most 16). The order is the skin weight index; they must exist in the character's skeleton.");
    field(changed, item, [s = std::string(jointsBuf_)](fmt::OcHairData& g) { g.scalpJoints = splitJoints(s); });
}

void HairGroomEditor::drawGenerate() {
    if (!ImGui::CollapsingHeader("Generator (GENERATE)")) return;
    using D = fmt::OcHairData;
    const fmt::OcHairGenerate& n = state_.generate;
    checkbox("Generate strands", n.present, [](D& g, bool v) { g.generate.present = v; },
             "A groom with stored strands uses them and ignores this; with none stored the strands are grown from the scalp when it loads.");
    if (!n.present) return;
    dragInt("Seed", static_cast<i32>(n.seed), 1.0f, 0, 2000000000, [](D& g, i32 v) { g.generate.seed = static_cast<u32>(std::max(v, 0)); }, "The same seed and scalp always grow the same strands.");
    dragInt("Strands", static_cast<i32>(n.strands), 10.0f, 1, static_cast<i32>(state_.maxRenderStrands), [](D& g, i32 v) { g.generate.strands = static_cast<u32>(std::max(v, 1)); },
            "Render strands. Draw cost follows this and the points per strand.");
    dragFloat("Length min (cm)", n.lengthCm[0], 0.1f, 0.1f, 400.0f, "%.1f", [](D& g, f32 v) { g.generate.lengthCm[0] = v; });
    dragFloat("Length max (cm)", n.lengthCm[1], 0.1f, 0.1f, 400.0f, "%.1f", [](D& g, f32 v) { g.generate.lengthCm[1] = v; });
    dragInt("Segments", static_cast<i32>(n.segments), 0.1f, 2, 254, [](D& g, i32 v) { g.generate.segments = static_cast<u32>(std::max(v, 2)); }, "Points per strand = segments + 1.");
    dragFloat("Curl (cm)", n.curlCm, 0.01f, 0.0f, 20.0f, "%.2f", [](D& g, f32 v) { g.generate.curlCm = v; }, "Amplitude of the helix around the strand; zero is straight.");
    dragFloat("Curl frequency", n.curlFrequency, 0.05f, 0.0f, 30.0f, "%.2f", [](D& g, f32 v) { g.generate.curlFrequency = v; }, "Turns per 10 cm of length.");
    dragInt("Clumps", static_cast<i32>(n.clumps), 1.0f, 0, 5000, [](D& g, i32 v) { g.generate.clumps = static_cast<u32>(std::max(v, 0)); }, "Guide strands the others gather around.");
    dragFloat("Clump radius (cm)", n.clumpRadiusCm, 0.05f, 0.0f, 20.0f, "%.2f", [](D& g, f32 v) { g.generate.clumpRadiusCm = v; });
    dragFloat("Clump tightness", n.clumpTightness, 0.01f, 0.0f, 1.0f, "%.2f", [](D& g, f32 v) { g.generate.clumpTightness = v; });
    dragFloat("Part width (cm)", n.partWidthCm, 0.05f, 0.0f, 10.0f, "%.2f", [](D& g, f32 v) { g.generate.partWidthCm = v; }, "Roots on either side of the part line are pushed away from it.");
    {
        float dir[3] = {n.partDirection[0], n.partDirection[1], n.partDirection[2]};
        const bool changed = ImGui::DragFloat3("Part direction", dir, 0.01f, -1.0f, 1.0f, "%.2f");
        const ItemState item = probe();
        field(changed, item, [dir](D& g) { for (int i = 0; i < 3; ++i) g.generate.partDirection[i] = dir[i]; });
    }
    {
        float st[3] = {n.style[0], n.style[1], n.style[2]};
        const bool changed = ImGui::DragFloat3("Style (fall direction)", st, 0.01f, -1.0f, 1.0f, "%.2f");
        const ItemState item = probe();
        if (item.hovered) ImGui::SetTooltip("The direction a long strand bends towards, engine axes (0 0 -1 falls straight down).");
        field(changed, item, [st](D& g) { for (int i = 0; i < 3; ++i) g.generate.style[i] = st[i]; });
    }
    dragFloat("Style weight", n.styleWeight, 0.01f, 0.0f, 1.0f, "%.2f", [](D& g, f32 v) { g.generate.styleWeight = v; });
    {
        const bool changed = ImGui::InputText("Root mask texture", maskBuf_, sizeof maskBuf_);
        const ItemState item = probe();
        if (item.hovered) ImGui::SetTooltip("A texture whose red channel weights where roots grow. Empty is uniform.");
        field(changed, item, [s = std::string(maskBuf_)](D& g) { g.generate.mask = s; });
    }
    dragInt("Material", static_cast<i32>(n.material), 0.1f, 0, 255, [](D& g, i32 v) { g.generate.material = static_cast<u32>(std::max(v, 0)); }, "The strand material the grown strands use.");
    dragInt("Max strands", static_cast<i32>(state_.maxRenderStrands), 10.0f, 1, 200000, [](D& g, i32 v) { g.maxRenderStrands = static_cast<u32>(std::max(v, 1)); },
            "MAXSTRANDS, the cap on render strands (the build refuses more).");
}

void HairGroomEditor::drawSim() {
    if (!ImGui::CollapsingHeader("Simulation (SIM)")) return;
    using D = fmt::OcHairData;
    const fmt::OcHairSim& s = state_.sim;
    dragFloat("Iterations per second", s.iterationsPerSecond, 1.0f, 1.0f, 4096.0f, "%.0f", [](D& g, f32 v) { g.sim.iterationsPerSecond = v; }, "Jolt's solver rate. Higher is stiffer and costlier.");
    dragFloat("Max delta time (s)", s.maxDeltaTime, 0.001f, 0.0001f, 1.0f, "%.4f", [](D& g, f32 v) { g.sim.maxDeltaTime = v; });
    {
        float grid[3] = {s.grid[0], s.grid[1], s.grid[2]};
        const bool changed = ImGui::DragFloat3("Grid cells", grid, 1.0f, 4.0f, 128.0f, "%.0f");
        const ItemState item = probe();
        if (item.hovered) ImGui::SetTooltip("The velocity and density grid Jolt uses to make strands move as a head of hair. 4 to 128 per axis.");
        field(changed, item, [grid](D& g) { for (int i = 0; i < 3; ++i) g.sim.grid[i] = grid[i]; });
    }
    dragFloat("Bounds padding (cm)", s.boundsPaddingCm, 0.1f, 0.0f, 100.0f, "%.1f", [](D& g, f32 v) { g.sim.boundsPaddingCm = v; });
    dragFloat("Simulated fraction", s.simFraction, 0.005f, 0.001f, 1.0f, "%.3f", [](D& g, f32 v) { g.sim.simFraction = v; }, "The share of render strands that are simulated; the rest follow the nearest simulated strand.");
    dragInt("Resample to (points)", static_cast<i32>(s.resample), 0.2f, 0, 255, [](D& g, i32 v) { g.sim.resample = v <= 0 ? 0u : static_cast<u32>(std::max(v, 3)); }, "0 keeps the strands as they are; otherwise every strand is resampled to this many points.");
    {
        float gr[3] = {s.initialGravity[0], s.initialGravity[1], s.initialGravity[2]};
        const bool changed = ImGui::DragFloat3("Initial gravity (cm/s^2)", gr, 1.0f, -5000.0f, 5000.0f, "%.0f");
        const ItemState item = probe();
        field(changed, item, [gr](D& g) { for (int i = 0; i < 3; ++i) g.sim.initialGravity[i] = gr[i]; });
    }
}

void HairGroomEditor::drawMaterials() {
    if (!ImGui::CollapsingHeader("Strand materials (MATERIAL)")) return;
    using D = fmt::OcHairData;
    if (state_.materials.empty()) return;
    selMaterial_ = std::clamp<int>(selMaterial_, 0, static_cast<int>(state_.materials.size()) - 1);
    {
        std::string current = "Material " + std::to_string(state_.materials[static_cast<usize>(selMaterial_)].index);
        if (ImGui::BeginCombo("Material", current.c_str())) {
            for (usize i = 0; i < state_.materials.size(); ++i) {
                const std::string label = "Material " + std::to_string(state_.materials[i].index);
                if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == selMaterial_)) selMaterial_ = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add##mat")) addMaterial();
    ImGui::SameLine();
    ImGui::BeginDisabled(state_.materials.size() <= 1);
    if (ImGui::SmallButton("Remove##mat")) removeMaterial(state_.materials[static_cast<usize>(selMaterial_)].index);
    ImGui::EndDisabled();
    if (selMaterial_ >= static_cast<int>(state_.materials.size())) return;
    const usize at = static_cast<usize>(selMaterial_);
    const u32 index = state_.materials[at].index;
    const fmt::OcHairMaterial& m = state_.materials[at];
    // Writes into the record with this index (it can move in the list when another is removed).
    auto mat = [index](D& g) -> fmt::OcHairMaterial* {
        for (fmt::OcHairMaterial& r : g.materials)
            if (r.index == index) return &r;
        return nullptr;
    };
    checkbox("Collision", m.collision, [mat](D& g, bool v) { if (auto* r = mat(g)) r->collision = v; });
    checkbox("Length constraints (LRA)", m.lra, [mat](D& g, bool v) { if (auto* r = mat(g)) r->lra = v; }, "Long range attachments keep a strand from stretching far from its root.");
    dragFloat("Linear damping", m.linearDamping, 0.05f, 0.0f, 100.0f, "%.2f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->linearDamping = v; }, "Raise it for hair that is too bouncy.");
    dragFloat("Angular damping", m.angularDamping, 0.05f, 0.0f, 100.0f, "%.2f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->angularDamping = v; });
    dragFloat("Max linear speed (cm/s)", m.maxLinearCmS, 5.0f, 1.0f, 100000.0f, "%.0f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->maxLinearCmS = v; }, "The velocity cap that keeps a sprint or a snap turn from detonating the hair.");
    dragFloat("Max angular speed (rad/s)", m.maxAngularRadS, 0.5f, 0.1f, 1000.0f, "%.1f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->maxAngularRadS = v; });
    dragFloat("Friction", m.friction, 0.005f, 0.0f, 10.0f, "%.3f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->friction = v; });
    dragFloat("Bend compliance", m.bendCompliance, 1.0e-9f, 0.0f, 1.0f, "%.3g", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->bendCompliance = v; }, "Larger is floppier. Raise it for stiff hair that will not bend.");
    dragFloat("Stretch compliance", m.stretchCompliance, 1.0e-10f, 0.0f, 1.0f, "%.3g", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->stretchCompliance = v; });
    dragFloat("Inertia multiplier", m.inertiaMul, 0.1f, 0.001f, 1000.0f, "%.2f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->inertiaMul = v; });
    dragFloat("Grid density force", m.gridDensityForce, 0.01f, 0.0f, 100.0f, "%.3f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->gridDensityForce = v; });
    dragFloat("Gravity preload", m.gravityPreload, 0.01f, 0.0f, 10.0f, "%.3f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->gravityPreload = v; });
    ImGui::TextDisabled("Gradients: value at the root-side and tip-side stops, and where along the strand each stop is.");
    gradientRow("Gravity", m.gravity, [mat](D& g, const fmt::OcHairGradient& v) { if (auto* r = mat(g)) r->gravity = v; });
    gradientRow("Radius (cm)", m.radiusCm, [mat](D& g, const fmt::OcHairGradient& v) { if (auto* r = mat(g)) r->radiusCm = v; }, "The collision radius of a strand vertex.");
    gradientRow("World influence", m.worldInfluence, [mat](D& g, const fmt::OcHairGradient& v) { if (auto* r = mat(g)) r->worldInfluence = v; }, "How much the head's motion drags the strand along (raise the minimum if hair shoots up on a turn).");
    gradientRow("Grid velocity", m.gridVelocity, [mat](D& g, const fmt::OcHairGradient& v) { if (auto* r = mat(g)) r->gridVelocity = v; });
    gradientRow("Global pose", m.globalPose, [mat](D& g, const fmt::OcHairGradient& v) { if (auto* r = mat(g)) r->globalPose = v; }, "The pull back to the rest shape. Lower it for hair that sags more.");
    gradientRow("Skin global pose", m.skinGlobalPose, [mat](D& g, const fmt::OcHairGradient& v) { if (auto* r = mat(g)) r->skinGlobalPose = v; });
    {
        f32 own = m.simFraction;
        bool use = own >= 0.0f;
        if (ImGui::Checkbox("Own simulated fraction", &use)) {
            const f32 value = use ? std::max(state_.sim.simFraction, 0.01f) : -1.0f;
            edit([mat, value](D& g) { if (auto* r = mat(g)) r->simFraction = value; });
        }
        if (use) dragFloat("Simulated fraction##mat", own, 0.005f, 0.001f, 1.0f, "%.3f", [mat](D& g, f32 v) { if (auto* r = mat(g)) r->simFraction = v; });
    }
}

void HairGroomEditor::drawLooks() {
    if (!ImGui::CollapsingHeader("Looks (LOOK)")) return;
    using D = fmt::OcHairData;
    if (state_.looks.empty()) return;
    selLook_ = std::clamp<int>(selLook_, 0, static_cast<int>(state_.looks.size()) - 1);
    {
        std::string current = "Look " + std::to_string(state_.looks[static_cast<usize>(selLook_)].material);
        if (ImGui::BeginCombo("Look", current.c_str())) {
            for (usize i = 0; i < state_.looks.size(); ++i) {
                const std::string label = "Look " + std::to_string(state_.looks[i].material);
                if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == selLook_)) selLook_ = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add##look")) addLook();
    ImGui::SameLine();
    ImGui::BeginDisabled(state_.looks.size() <= 1);
    if (ImGui::SmallButton("Remove##look")) removeLook(state_.looks[static_cast<usize>(selLook_)].material);
    ImGui::EndDisabled();
    if (selLook_ >= static_cast<int>(state_.looks.size())) return;
    const u32 index = state_.looks[static_cast<usize>(selLook_)].material;
    const fmt::OcHairLook& l = state_.looks[static_cast<usize>(selLook_)];
    auto look = [index](D& g) -> fmt::OcHairLook* {
        for (fmt::OcHairLook& r : g.looks)
            if (r.material == index) return &r;
        return nullptr;
    };
    // The root-to-tip swatch, linear colours shown as sRGB approximately (gamma 2.2).
    {
        std::vector<std::array<f32, 3>> steps;
        hairmodel::lookGradient(l, 16, steps);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const f32 w = std::max(ImGui::GetContentRegionAvail().x, 64.0f), h = 14.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto col = [](const std::array<f32, 3>& c) {
            auto enc = [](f32 v) { return static_cast<int>(std::clamp(std::pow(std::max(v, 0.0f), 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f + 0.5f); };
            return IM_COL32(enc(c[0]), enc(c[1]), enc(c[2]), 255);
        };
        for (usize i = 0; i + 1 < steps.size(); ++i) {
            const f32 x0 = p.x + w * static_cast<f32>(i) / static_cast<f32>(steps.size() - 1);
            const f32 x1 = p.x + w * static_cast<f32>(i + 1) / static_cast<f32>(steps.size() - 1);
            dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + h), col(steps[i]), col(steps[i + 1]), col(steps[i + 1]), col(steps[i]));
        }
        ImGui::Dummy(ImVec2(w, h + 2.0f));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Root colour to tip colour along the strand.");
    }
    auto color = [&](const char* label, const f32* c, const std::function<void(fmt::OcHairLook&, const f32*)>& set, const char* tip) {
        float v[3] = {c[0], c[1], c[2]};
        const bool changed = ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        const ItemState item = probe();
        if (tip && item.hovered) ImGui::SetTooltip("%s", tip);
        field(changed, item, [look, set, v](D& g) { if (auto* r = look(g)) set(*r, v); });
    };
    color("Root colour", l.baseColor, [](fmt::OcHairLook& r, const f32* v) { for (int i = 0; i < 3; ++i) r.baseColor[i] = v[i]; }, "Linear colour at the root.");
    color("Tip colour", l.tipColor, [](fmt::OcHairLook& r, const f32* v) { for (int i = 0; i < 3; ++i) r.tipColor[i] = v[i]; }, "Linear colour at the tip. Tips lighter than roots read as natural.");
    color("Highlight tint", l.tint, [](fmt::OcHairLook& r, const f32* v) { for (int i = 0; i < 3; ++i) r.tint[i] = v[i]; }, "The colour of the transmission (TT) and second reflection (TRT) highlights.");
    dragFloat("Colour variation", l.colorVariation, 0.005f, 0.0f, 1.0f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->colorVariation = v; }, "Per-strand brightness variation.");
    dragFloat("Root width (cm)", l.widthRootCm, 0.002f, 0.001f, 2.0f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->widthRootCm = v; });
    dragFloat("Tip width (cm)", l.widthTipCm, 0.002f, 0.0f, 2.0f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->widthTipCm = v; });
    dragFloat("Longitudinal roughness (beta)", l.roughness, 0.005f, 0.02f, 1.0f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->roughness = v; }, "Highlights too sharp: raise it.");
    dragFloat("Cuticle tilt (rad)", l.shift, 0.002f, -0.35f, 0.35f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->shift = v; }, "Shifts the two highlights along the strand.");
    dragFloat("TT weight", l.tt, 0.01f, 0.0f, 4.0f, "%.2f", [look](D& g, f32 v) { if (auto* r = look(g)) r->tt = v; });
    dragFloat("TRT weight", l.trt, 0.01f, 0.0f, 4.0f, "%.2f", [look](D& g, f32 v) { if (auto* r = look(g)) r->trt = v; });
    dragFloat("Azimuthal roughness", l.azimuthRough, 0.005f, 0.02f, 1.0f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->azimuthRough = v; });
    dragFloat("Scatter", l.scatter, 0.005f, 0.0f, 1.0f, "%.3f", [look](D& g, f32 v) { if (auto* r = look(g)) r->scatter = v; }, "Multiple-scatter ambient. Hair too dark in shade: raise it.");
    ImGui::TextDisabled("The scene uses the first look (a .ocmat override on the Hair component replaces it).");
}

void HairGroomEditor::drawLod() {
    if (!ImGui::CollapsingHeader("Level of detail (LOD)")) return;
    using D = fmt::OcHairData;
    const fmt::OcHairLod& lod = state_.lod;
    for (int i = 0; i < 3; ++i) {
        const std::string label = "Level " + std::to_string(i + 1) + " from (cm)";
        dragFloat(label.c_str(), lod.distances[i], 5.0f, 1.0f, 100000.0f, "%.0f", [i](D& g, f32 v) { g.lod.distances[i] = v; }, "Distances increase; the next level starts here.");
    }
    for (int i = 0; i < 4; ++i) {
        const std::string label = "Level " + std::to_string(i) + " strands drawn";
        dragFloat(label.c_str(), lod.strandFractions[i], 0.005f, 0.01f, 1.0f, "%.2f", [i](D& g, f32 v) { g.lod.strandFractions[i] = v; }, "A fraction of the render strands; each level draws no more than the one before.");
    }
    const hairmodel::GroomBudget b = budget();
    const auto rows = hairmodel::lodRows(lod, b.renderStrands);
    for (usize i = 0; i < rows.size(); ++i)
        ImGui::TextDisabled("Level %u: from %.0f cm, %llu strands (%.0f%%)", static_cast<unsigned>(i), static_cast<double>(rows[i].fromCm),
                            static_cast<unsigned long long>(rows[i].strandsDrawn), static_cast<double>(rows[i].fraction) * 100.0);
}

void HairGroomEditor::drawProxies() {
    if (!ImGui::CollapsingHeader("Collision proxies (PROXY)")) return;
    using D = fmt::OcHairData;
    ImGui::TextWrapped("Jolt's hair collides with convex hulls only, so the head, neck and shoulders are stood in for by hulls that follow bones. "
                       "With none listed here the engine derives a humanoid set from the skeleton's joint names.");
    if (ImGui::SmallButton("Add proxy")) addProxy(hairmodel::newProxy(state_.proxies));
    ImGui::SameLine();
#if AVER_MODULE_HAIR
    if (ImGui::SmallButton("Use humanoid starter set")) useStarterProxies();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces the list with a head sphere, a neck capsule, two shoulder capsules and a spine box on the usual bone names. Rename the bones to match your rig.");
    ImGui::SameLine();
#endif
    ImGui::BeginDisabled(state_.proxies.empty());
    if (ImGui::SmallButton("Remove selected") && selProxy_ >= 0 && static_cast<usize>(selProxy_) < state_.proxies.size()) removeProxy(static_cast<usize>(selProxy_));
    ImGui::EndDisabled();
    for (usize i = 0; i < state_.proxies.size(); ++i) {
        const fmt::OcHairProxy& p = state_.proxies[i];
        const std::string label = p.name + " (" + hairmodel::proxyShapeName(p.shape) + " on " + p.bone + ")##proxy" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == selProxy_)) selProxy_ = static_cast<int>(i);
    }
    if (state_.proxies.empty() || selProxy_ < 0 || static_cast<usize>(selProxy_) >= state_.proxies.size()) return;
    const usize at = static_cast<usize>(selProxy_);
    const fmt::OcHairProxy& p = state_.proxies[at];
    ImGui::Separator();
    {
        char name[64], bone[64];
        copyText(name, sizeof name, p.name);
        copyText(bone, sizeof bone, p.bone);
        bool changed = ImGui::InputText("Name", name, sizeof name);
        ItemState item = probe();
        field(changed, item, [at, s = std::string(name)](D& g) { if (at < g.proxies.size()) g.proxies[at].name = s; });
        changed = ImGui::InputText("Bone", bone, sizeof bone);
        item = probe();
        if (item.hovered) ImGui::SetTooltip("The skeleton bone the hull follows (matched without regard to case).");
        field(changed, item, [at, s = std::string(bone)](D& g) { if (at < g.proxies.size()) g.proxies[at].bone = s; });
    }
    {
        int shape = static_cast<int>(p.shape);
        static const char* const shapes[3] = {"sphere", "capsule", "box"};
        const bool changed = ImGui::Combo("Shape", &shape, shapes, 3);
        const ItemState item = probe();
        field(changed, item, [at, shape](D& g) { if (at < g.proxies.size()) g.proxies[at].shape = static_cast<fmt::OcHairProxyShape>(shape); });
    }
    {
        float size[3] = {p.sizeCm[0], p.sizeCm[1], p.sizeCm[2]};
        const bool changed = ImGui::DragFloat3("Size (cm)", size, 0.1f, 0.0f, 400.0f, "%.1f");
        const ItemState item = probe();
        if (item.hovered) ImGui::SetTooltip("Sphere: radius. Capsule: radius, half height of the straight part. Box: half extents.");
        field(changed, item, [at, size](D& g) { if (at < g.proxies.size()) for (int i = 0; i < 3; ++i) g.proxies[at].sizeCm[i] = size[i]; });
    }
    {
        float off[3] = {p.offsetCm[0], p.offsetCm[1], p.offsetCm[2]};
        const bool changed = ImGui::DragFloat3("Offset (cm)", off, 0.1f, -400.0f, 400.0f, "%.1f");
        const ItemState item = probe();
        if (item.hovered) ImGui::SetTooltip("From the bone, in the bone's frame.");
        field(changed, item, [at, off](D& g) { if (at < g.proxies.size()) for (int i = 0; i < 3; ++i) g.proxies[at].offsetCm[i] = off[i]; });
    }
}

void HairGroomEditor::drawStrands() {
    if (!ImGui::CollapsingHeader("Stored strands and curve import")) return;
    if (strands_.empty()) ImGui::TextDisabled("No stored strands: the groom is grown from GENERATE when it loads.");
    else ImGui::TextDisabled("%zu stored strands, %zu points. They win over GENERATE.", strands_.strandCount(), strands_.pointCount());
#if AVER_MODULE_HAIR
    ImGui::InputText("glTF with curves", importBuf_, sizeof importBuf_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An absolute path to a .gltf or .glb with LINES or LINE_STRIP strands, in the same space as the scalp mesh.");
    ImGui::SameLine();
    if (ImGui::Button("Import curves")) {
        std::string why;
        if (importCurves(importBuf_, &why)) importMessage_ = "Imported " + std::to_string(strands_.strandCount()) + " strands.";
        else importMessage_ = "Import failed: " + why;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces the stored strands. Not an undo step; the file is not changed until you save.");
#endif
    ImGui::BeginDisabled(strands_.empty());
    if (ImGui::Button("Use GENERATE instead (drop stored strands)")) clearStoredStrands();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drops the stored strands. Not an undo step; the file is not changed until you save.");
    if (!importMessage_.empty()) ImGui::TextWrapped("%s", importMessage_.c_str());
}

void HairGroomEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    // Rebuild the preview at most every ~120 ms while edits keep arriving, and at once when idle.
    if (previewStale_) {
        const f64 now = ImGui::GetTime() * 1000.0;
        if (now >= previewNextMs_) {
            rebuildPreview();
            previewNextMs_ = now + 120.0;
        }
    }
    drawToolbar();
    ImGui::Separator();
    drawAdvice();
    drawPreview();
    drawScalp();
    drawGenerate();
    drawStrands();
    drawSim();
    drawMaterials();
    drawLooks();
    drawLod();
    drawProxies();
}

#else   // AVER_WITH_IMGUI

// The headless build still gets load, save, dirty, undo and every edit; only the window is absent.
void HairGroomEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

bool writeStarterHairGroom(const std::string& absPath, std::string* why) {
    std::error_code ec;
    if (std::filesystem::exists(absPath, ec)) {
        if (why) *why = "the file already exists";
        return false;
    }
    fmt::OcHairData g = hairmodel::starterGroom();
    hairmodel::sanitize(g);
    return fmt::saveOcHair(absPath, g, why);
}

std::unique_ptr<AssetEditor> makeHairGroomEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ochair") return nullptr;
    return std::make_unique<HairGroomEditor>(path);
}

// ================================================================================ the Hair Details section

#if AVER_WITH_IMGUI && AVER_MODULE_HAIR_SCENE

void drawHairSection(const HairSectionEnv& env, void* worldPtr, u32 entity) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const u32 type = hair::HairSystem::hairType();
    auto* c = type && w.valid(e) ? w.component<hair::CHair>(e, type) : nullptr;
    if (!c) return;
    if (!ImGui::CollapsingHeader("Hair", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (env.assetRow) env.assetRow("asset", "Groom (.ochair)", c->asset);

    // The scales read 0 as 1 (a zero-filled component is the default), so the widgets show 1 for 0 and write what the user typed.
    const auto scale = [&](const char* label, f32& field, f32 lo, f32 hi, const char* tip) {
        f32 v = field > 0.0f ? field : 1.0f;
        env.gestureEdit(ImGui::DragFloat(label, &v, 0.01f, lo, hi, "%.2f"), [&] { field = v; });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    scale("Width scale", c->widthScale, 0.1f, 8.0f, "Multiplies the width of every strand (the groom's LOOK width times this).");
    scale("Density scale", c->densityScale, 0.05f, 1.0f, "The fraction of the render strands drawn, on top of the distance levels.");
    scale("Wind scale", c->windScale, 0.0f, 8.0f, "Multiplies the world wind this groom feels.");
    scale("Stiffness scale", c->stiffnessScale, 0.1f, 10.0f, "Divides the bend and stretch compliance of every material of the groom. Shared by every instance of the groom.");
    f32 bias = c->lodBias;
    env.gestureEdit(ImGui::DragFloat("LOD bias", &bias, 0.05f, -3.0f, 3.0f, "%.2f"), [&] { c->lodBias = bias; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Steps added to the distance level: positive draws fewer, fatter strands.");
    f32 tele = c->teleportCm;
    env.gestureEdit(ImGui::DragFloat("Teleport distance (cm)", &tele, 1.0f, 0.0f, 100000.0f, tele > 0.0f ? "%.0f" : "default"), [&] { c->teleportCm = tele; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A head that moves further than this in one frame restarts the hair from its default pose.");

    const auto flag = [&](const char* label, u32 bit, bool inverted, const char* tip) {
        bool on = inverted ? (c->flags & bit) == 0 : (c->flags & bit) != 0;
        env.instantEdit(ImGui::Checkbox(label, &on), [&] { c->flags = (on != inverted) ? (c->flags | bit) : (c->flags & ~bit); });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    flag("Collides", hair::kHairNoCollision, true, "Collides with the head, neck and shoulder proxies (and any hull body on the proxy layer).");
    flag("Casts shadows", hair::kHairNoShadow, true, "Whether the strands cast shadow-map shadows.");
    flag("Ray traced", hair::kHairRayTraced, false, "Puts the hair in the ray-tracing structure (reflections, GI, RT shadows). Off by default: it adds a ribbon BLAS refit every frame.");
    flag("Feels wind", hair::kHairNoWind, true, "Whether the world wind moves this groom.");
    flag("Hidden", hair::kHairHidden, false, "Neither drawn nor expanded; the simulation still follows the cadence rules.");

    hairmodel::HairFacts facts;
    facts.hasAsset = c->asset != 0;
    facts.hasSkeleton = w.hasComponent(e, scene::kComponentSkeletalMesh);
    facts.playing = env.playing;
    facts.outState = c->outState;
    facts.renderStrands = c->outRenderStrands;
    facts.simStrands = c->outSimStrands;
    facts.computeHost = env.computeHost;
    const std::string advice = hairmodel::advice(facts);
    if (!advice.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", advice.c_str());
    const std::string state = hairmodel::stateText(facts);
    if (!state.empty()) ImGui::TextDisabled("%s", state.c_str());

    if (env.playing) {
        if (ImGui::SmallButton("Teleport (default pose)")) hair::hairSystem().teleport(e);
    }
}

#endif

} // namespace aver::editor
