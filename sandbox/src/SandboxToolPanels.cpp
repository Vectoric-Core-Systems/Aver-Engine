// Editor: the Place Actors, Message Log and Undo History windows (editor polish step 9).
// Dockable windows reached from the Window menu; the filtering and jump arithmetic live in PanelModels.hpp.

#include "SandboxApp.hpp"
#include "PanelModels.hpp"
#include "aver/rhi/PrimitiveMeshes.hpp"

#include <set>

namespace aver {
#if AVER_WITH_IMGUI

// ---- Place Actors ------------------------------------------------------------------------------

static std::vector<editor::PlaceItem> placeCatalogue() {
    std::vector<editor::PlaceItem> v;
    v.push_back({"cube", "Cube", "Basic", ICON_ACTOR_MESH});
    v.push_back({"sphere", "Sphere", "Basic", ICON_SPHERE});
    for (const rhi::primitives::BuiltinPrimitive& p : rhi::primitives::kBuiltinPrimitives)
        v.push_back({std::string("prim:") + p.path, p.label, "Shapes", ICON_ACTOR});
#if AVER_MODULE_SCENE
    v.push_back({"light.point", "Point Light", "Lights", ICON_LIGHT});
    v.push_back({"light.spot", "Spot Light", "Lights", ICON_SPOT_LIGHT});
    v.push_back({"light.rect", "Rect Light", "Lights", ICON_RECT_LIGHT});
    v.push_back({"decal", "Decal", "Effects", ICON_DECAL});
#endif
    v.push_back({"postvolume", "Post Process Volume", "Volumes", ICON_POST_PROCESS});
    v.push_back({"playerstart", "Player Start", "Gameplay", ICON_PLAYER_START});
    return v;
}

void SandboxApp::placeActorKind(Engine& e, const std::string& kind, const Vec3* dropAt, bool onSurface) {
    // The spawn functions read spawnPoint(); an override lives only for this call.
    struct Reset { std::optional<Vec3>& o; ~Reset() { o.reset(); } } reset{rset_.spawnAtOverride_};
    const auto at = [&](f32 lift) {
        if (dropAt) rset_.spawnAtOverride_ = Vec3{dropAt->x, dropAt->y, dropAt->z + (onSurface ? lift : 0.0f)};
    };
    const auto primitive = [&](const char* path, const char* label) {
        const rhi::primitives::BuiltinPrimitive* p = rhi::primitives::findBuiltinPrimitive(path);
        at(p ? p->spawnScale : kEditorCubeHalf);   // rest it on the surface rather than bury its origin
        spawnPrimitive(e, path, label);
    };
    if (kind == "cube") primitive("Meshes/cube.ocmesh", "Cube");
    else if (kind == "sphere") primitive("Meshes/sphere.ocmesh", "Sphere");
    else if (kind.rfind("prim:", 0) == 0) {
        for (const rhi::primitives::BuiltinPrimitive& p : rhi::primitives::kBuiltinPrimitives)
            if (kind.compare(5, std::string::npos, p.path) == 0) { primitive(p.path, p.label); break; }
    } else if (kind == "playerstart") { at(0.0f); addPlayerStart(e); }
    else if (kind == "postvolume") {
        postVolumeTools_.addAtCamera(levelHeader_.postVolumes, dropAt ? *dropAt : viewPos());
        selectionUi_.sel_ = SelectionState::kSelPostVolume; selectionUi_.selEntity_ = kInvalidId;
        markLevelUnsaved();
    }
#if AVER_MODULE_SCENE
    else if (kind == "light.point") { at(0.0f); spawnLightAtCamera(scene::kLightPoint); }
    else if (kind == "light.spot")  { at(0.0f); spawnLightAtCamera(scene::kLightSpot); }
    else if (kind == "light.rect")  { at(0.0f); spawnLightAtCamera(scene::kLightRect); }
    else if (kind == "decal")       { at(0.0f); spawnDecalAtCamera(); }
#endif
}

void SandboxApp::buildPlaceActorsPanel(Engine& e) {
    if (!ImGui::Begin(ICON_ADD " Place Actors###Place Actors", &panels_.showPlaceActors_)) { ImGui::End(); return; }
    static const std::vector<editor::PlaceItem> all = placeCatalogue();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##placeSearch", ICON_SEARCH " Search actors", panels_.placeQuery_, sizeof panels_.placeQuery_);
    ImGui::Separator();
    const std::vector<editor::PlaceItem> shown = editor::filterPlaceItems(all, panels_.placeQuery_, "");
    for (const std::string& cat : editor::placeCategories(shown)) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (!ImGui::CollapsingHeader(cat.c_str())) continue;
        for (const editor::PlaceItem& it : shown) {
            if (it.category != cat) continue;
            const std::string label = (it.icon.empty() ? std::string() : it.icon + " ") + it.label + "###place_" + it.kind;
            if (ImGui::Selectable(label.c_str())) placeActorKind(e, it.kind, nullptr, false);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(kPlaceActorDragDropType, it.kind.c_str(), it.kind.size() + 1);
                ImGui::TextUnformatted(it.label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to place in front of the camera, or drag into the viewport.");
        }
    }
    if (shown.empty()) ImGui::TextDisabled("Nothing matches.");
    ImGui::End();
}

// ---- Message Log -------------------------------------------------------------------------------

void SandboxApp::buildMessageLogPanel() {
    if (!ImGui::Begin(ICON_LOG " Message Log###Message Log", &panels_.showMessageLog_)) { ImGui::End(); return; }
    editor::LogFilter f;
    f.minLevel = panels_.msgLogLevel_ == 4 ? LogLevel::Critical : panels_.msgLogLevel_ == 3 ? LogLevel::Error
               : panels_.msgLogLevel_ == 2 ? LogLevel::Warn : panels_.msgLogLevel_ == 1 ? LogLevel::Info : LogLevel::Trace;
    f.category = panels_.msgLogCategory_;
    f.query = panels_.msgLogQuery_;

    // Read-only over the log buffer the sink fills; this panel never logs, so it cannot feed itself.
    std::vector<u32> shown;
    std::set<std::string> categories;
    usize total = 0;
    std::string copyText;
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        total = drawerUi_.logLines_.size();
        for (usize i = 0; i < drawerUi_.logLines_.size(); ++i) {
            const LogLine& ln = drawerUi_.logLines_[i];
            const std::string_view cat = editor::logCategoryOf(ln.text);
            if (!cat.empty()) categories.insert(std::string(cat));
            if (editor::logLineMatches(f, ln.level, ln.text)) shown.push_back(static_cast<u32>(i));
        }
        ImGui::BeginDisabled(shown.empty());
        const bool wantCopy = ImGui::SmallButton(ICON_COPY " Copy");
        ImGui::EndDisabled();
        if (wantCopy) {
            for (const u32 i : shown) { copyText += drawerUi_.logLines_[i].text; copyText += '\n'; }
        }
    }
    if (!copyText.empty()) ImGui::SetClipboardText(copyText.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_CLEAR_ALL " Clear")) {
        std::lock_guard<std::mutex> lk(logMutex_);
        drawerUi_.logFirstSerial_ += drawerUi_.logLines_.size();
        drawerUi_.logLines_.clear();
        drawerUi_.logMultiRowCount_ = 0;
        drawerUi_.logSelAnchor_ = drawerUi_.logSelEnd_ = -1;
        shown.clear();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clears the shared log buffer (the Output Log drawer too).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f * viewState_.dpi_);
    ImGui::Combo("##msgLevel", &panels_.msgLogLevel_, "All\0Info+\0Warn+\0Error+\0Critical+\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f * viewState_.dpi_);
    if (ImGui::BeginCombo("##msgCat", panels_.msgLogCategory_.empty() ? "All categories" : panels_.msgLogCategory_.c_str())) {
        if (ImGui::Selectable("All categories", panels_.msgLogCategory_.empty())) panels_.msgLogCategory_.clear();
        for (const std::string& c : categories)
            if (ImGui::Selectable(c.c_str(), c == panels_.msgLogCategory_)) panels_.msgLogCategory_ = c;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##msgSearch", ICON_SEARCH " Filter", panels_.msgLogQuery_, sizeof panels_.msgLogQuery_);
    ImGui::TextDisabled("%zu of %zu messages", shown.size(), total);
    ImGui::Separator();

    ImGui::BeginChild("##msgscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        bool multi = false;
        for (const u32 i : shown)
            if (i < drawerUi_.logLines_.size() && drawerUi_.logLines_[i].rows > 1) { multi = true; break; }
        if (!multi) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()), std::floor(ImGui::GetTextLineHeightWithSpacing()));
            while (clipper.Step())
                for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                    const u32 i = shown[static_cast<usize>(k)];
                    if (i < drawerUi_.logLines_.size()) drawLogLine(drawerUi_.logLines_[i].level, drawerUi_.logLines_[i].text.c_str());
                }
        } else {
            // Multi-row lines break the clipper's fixed row height: draw the newest few hundred whole.
            const usize first = shown.size() > 600 ? shown.size() - 600 : 0;
            for (usize k = first; k < shown.size(); ++k)
                if (shown[k] < drawerUi_.logLines_.size()) drawLogLine(drawerUi_.logLines_[shown[k]].level, drawerUi_.logLines_[shown[k]].text.c_str());
        }
        if (atBottom) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Undo History ------------------------------------------------------------------------------

void SandboxApp::buildUndoHistoryPanel() {
    if (!ImGui::Begin(ICON_RESET " Undo History###Undo History", &panels_.showUndoHistory_)) { ImGui::End(); return; }
    const auto kindName = [](EditCmd::Kind k) -> const char* {
        switch (k) {
            case EditCmd::Kind::Transform: return "Transform";
            case EditCmd::Kind::Create: case EditCmd::Kind::CreateObj: return "Add";
            case EditCmd::Kind::Destroy: case EditCmd::Kind::DestroyObj: return "Delete";
            case EditCmd::Kind::LandscapeStroke: case EditCmd::Kind::FoliageStroke: return "Paint";
            case EditCmd::Kind::Reparent: return "Reparent";
            case EditCmd::Kind::Material: return "Material";
            case EditCmd::Kind::Rename: return "Rename";
            case EditCmd::Kind::RemoveComponent: return "Remove Component";
            case EditCmd::Kind::Visibility: return "Visibility";
            case EditCmd::Kind::Collision: return "Collision";
            case EditCmd::Kind::Animation: return "Animation";
            case EditCmd::Kind::Prefab: return "Prefab";
            case EditCmd::Kind::LevelInstance: return "Level Instance";
            case EditCmd::Kind::LandscapeWeights: return "Landscape Layers";
            case EditCmd::Kind::WaterBodies: return "Water";
            case EditCmd::Kind::PlacedComponent: return "Component";
            case EditCmd::Kind::OutlinerMeta: return "Outliner";
            case EditCmd::Kind::DataLayers: return "Data Layers";
        }
        return "Edit";
    };
    const auto describe = [&](const EditCmd& c) {
        std::string s = kindName(c.kind);
        const std::string& what = !c.label.empty() ? c.label : c.className;
        if (!what.empty()) s += ": " + what;
        return s;
    };
    const usize u = undoStack_.size(), r = redoStack_.size();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::SmallButton(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::SmallButton(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu done, %zu undone", u, r);
    ImGui::Separator();

    int clicked = -2;   // -2: nothing; -1: the initial state; else a row
    ImGui::BeginChild("##undoscroll", ImVec2(0, 0), false);
    if (ImGui::Selectable("Initial state", u == 0)) clicked = -1;
    for (usize i = 0; i < u + r; ++i) {
        const bool done = i < u;
        const EditCmd& c = done ? undoStack_[i] : redoStack_[r - 1 - (i - u)];
        const std::string label = describe(c) + "###uh" + std::to_string(i);
        if (!done) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label.c_str(), done && i + 1 == u)) clicked = static_cast<int>(i);
        if (!done) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::End();

    if (clicked != -2) {
        const int steps = editor::undoHistorySteps(u, r, clicked);
        for (int k = 0; k < -steps && canUndo(); ++k) undo();
        for (int k = 0; k < steps && canRedo(); ++k) redo();
    }
}

#endif
} // namespace aver
