// The .ocfoliage editor tab. See the header for the three-line SandboxApp hook and for why this tab
// has no preview pane, unlike ParticleEditor.
#include "FoliageTypeEditor.hpp"

#include "EditorKeybinds.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "imgui.h"
#endif

namespace aver::editor {

FoliageTypeEditor::FoliageTypeEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void FoliageTypeEditor::loadFromDisk() {
    std::string why;
    fmt::OcFoliageData loaded;
    if (!fmt::loadOcFoliage(path_, loaded, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    type_ = std::move(loaded);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
    syncEditBuffers();
}

void FoliageTypeEditor::syncEditBuffers() {
    std::snprintf(meshBuf_, sizeof meshBuf_, "%s", type_.meshPath.c_str());
    std::snprintf(materialBuf_, sizeof materialBuf_, "%s", type_.material.c_str());
}

std::string FoliageTypeEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor
    // whose dirty() is true (AssetEditor.cpp) -- a '*' here would double it, the same bug Bt/Sound/
    // ParticleEditor's own title() comments already document for this codebase.
    return std::filesystem::path(path_).filename().string() + "###foliagetype:" + path_;
}

bool FoliageTypeEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!fmt::saveOcFoliage(path_, type_, why)) return false;
    dirty_ = false;
    return true;
}

void FoliageTypeEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS -- Bt/Sound/ParticleEditor's own rule: reloading here would discard
    // what the author typed because something else touched the file, which an editor must never do
    // on its own.
    if (dirty_) {
        AVER_WARN("[FoliageTypeEditor] '{}' changed on disk, but this tab has unsaved edits -- "
                  "keeping them", path_);
        return;
    }
    loadFromDisk();
}

// ---- undo -----------------------------------------------------------------------------------------

void FoliageTypeEditor::pushUndo() {
    history_.push(type_);
}

void FoliageTypeEditor::undo() {
    if (!history_.undo(type_)) return;
    dirty_ = true;
    syncEditBuffers();   // meshPath/material may have just changed under the text fields
}

void FoliageTypeEditor::redo() {
    if (!history_.redo(type_)) return;
    dirty_ = true;
    syncEditBuffers();
}

// ---- field edits ----------------------------------------------------------------------------------
//
// Each clamps to whatever OcFoliageData::valid() enforces at save time (modules/formats/src/
// OcFoliage.cpp), so a value typed in the UI can never be one the save path would go on to reject --
// ParticleEditor's own setters follow the identical rule for the identical reason.

void FoliageTypeEditor::setMeshPath(std::string meshPath) {
    if (!loaded_ || type_.meshPath == meshPath) return;
    pushUndo();
    type_.meshPath = std::move(meshPath);
    dirty_ = true;
}

void FoliageTypeEditor::setMaterial(std::string material) {
    if (!loaded_ || type_.material == material) return;
    pushUndo();
    type_.material = std::move(material);
    dirty_ = true;
}

void FoliageTypeEditor::setScaleRange(f32 scaleMin, f32 scaleMax) {
    if (!loaded_) return;
    pushUndo();
    // > 0, matching OcFoliageData::valid()'s own floor -- a zero or negative multiplier is a
    // degenerate mesh, not a small one.
    scaleMin = std::max(0.01f, scaleMin);
    scaleMax = std::max(scaleMin, scaleMax);   // valid() requires scaleMax >= scaleMin
    type_.scaleMin = scaleMin;
    type_.scaleMax = scaleMax;
    dirty_ = true;
}

void FoliageTypeEditor::setWeight(f32 weight) {
    if (!loaded_) return;
    pushUndo();
    type_.weight = weight;   // <= 0 is a valid, meaningful "never picked" -- ScatterSpecies' own rule
    dirty_ = true;
}

void FoliageTypeEditor::setRandomizeYaw(bool on) {
    if (!loaded_ || type_.randomizeYaw == on) return;
    pushUndo();
    type_.randomizeYaw = on;
    dirty_ = true;
}

void FoliageTypeEditor::setCollisionRadiusCm(f32 radiusCm) {
    if (!loaded_) return;
    pushUndo();
    type_.collisionRadiusCm = std::max(0.0f, radiusCm);   // valid()'s own floor; 0 disables the check
    dirty_ = true;
}

void FoliageTypeEditor::setAlignToNormal(bool on) {
    if (!loaded_ || type_.alignToNormal == on) return;
    pushUndo();
    type_.alignToNormal = on;
    dirty_ = true;
}

// ---- drawing --------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

void FoliageTypeEditor::drawParams() {
    ImGui::SeparatorText("Mesh");
    if (ImGui::InputText("Mesh path", meshBuf_, sizeof meshBuf_)) setMeshPath(meshBuf_);
    ImGui::TextDisabled("Content-relative path to the .ocmesh this type places.");
    if (ImGui::InputText("Material", materialBuf_, sizeof materialBuf_)) setMaterial(materialBuf_);
    ImGui::TextDisabled("Empty uses the mesh's own cooked material.");

    ImGui::SeparatorText("Placement");
    {
        f32 scaleMin = type_.scaleMin, scaleMax = type_.scaleMax;
        bool changed = false;
        changed |= ImGui::DragFloat("Scale min", &scaleMin, 0.01f, 0.01f, 10.0f, "%.2f");
        changed |= ImGui::DragFloat("Scale max", &scaleMax, 0.01f, 0.01f, 10.0f, "%.2f");
        if (changed) setScaleRange(scaleMin, scaleMax);

        bool randomizeYaw = type_.randomizeYaw;
        if (ImGui::Checkbox("Randomize yaw", &randomizeYaw)) setRandomizeYaw(randomizeYaw);

        bool alignToNormal = type_.alignToNormal;
        if (ImGui::Checkbox("Align to slope", &alignToNormal)) setAlignToNormal(alignToNormal);
        ImGui::TextDisabled("Tilts each instance to the sampled terrain normal instead of upright.");
    }

    ImGui::SeparatorText("Palette");
    {
        f32 weight = type_.weight;
        if (ImGui::DragFloat("Weight", &weight, 0.05f, 0.0f, 100.0f, "%.2f")) setWeight(weight);
        ImGui::TextDisabled("Relative pick chance among the palette's other enabled types. "
                            "0 disables this type without removing it.");

        f32 radius = type_.collisionRadiusCm;
        if (ImGui::DragFloat("Min spacing (cm)", &radius, 1.0f, 0.0f, 5000.0f, "%.0f"))
            setCollisionRadiusCm(radius);
        ImGui::TextDisabled("0 disables the check -- for grass and other overlap-tolerant fill.");
    }
}

void FoliageTypeEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[FoliageTypeEditor] save failed for '{}': {}", path_, why);
    }
    // Ctrl+Z / Ctrl+Y reach the same undo()/redo() the buttons below call: guarded by canUndo()/
    // canRedo() the way the buttons are, and skipped while an InputText has focus -- it has its own
    // Ctrl+Z, and WantTextInput is how GraphEditor.cpp's canvas tells the two apart.
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

    ImGui::Separator();
    drawParams();
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's own target, which deliberately leaves AVER_WITH_IMGUI
// undefined -- see tests/editor/CMakeLists.txt) still gets load/save/dirty/undo/every field setter;
// only the window is absent. Bt/Sound/ParticleEditor's own #else branches do exactly this.
void FoliageTypeEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeFoliageTypeEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocfoliage") return nullptr;
    return std::make_unique<FoliageTypeEditor>(path);
}

} // namespace aver::editor
