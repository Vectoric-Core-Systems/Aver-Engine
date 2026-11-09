// Editor: Window > Data Layers (WAVE2_UE W10 step 4). The rules are DataLayerModel.hpp and the widgets DataLayersPanel.cpp; this is the part of SandboxApp
// that joins them to the scene: the selection, the undo stack, the level's DATALAYER records and each placement's `layers=`, and the editor-only
// show / load toggles (which reuse the session hide, so they are never saved and an H-hide is left alone).

#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE

#include <algorithm>

namespace aver {

void SandboxApp::drawDataLayersPanel() {
    if (!showDataLayers_) return;
    editor::DataLayersHost host;
    host.selection = [this]() {
        std::vector<editor::dl::Key> keys;
        for (const scene::Entity e : selectedEntities()) keys.push_back(editIdFor(e));
        return keys;
    };
    host.selectKeys = [this](const std::vector<editor::dl::Key>& keys) {
        std::vector<scene::Entity> ents;
        for (const editor::dl::Key k : keys) {
            const scene::Entity e = entityForEdit(static_cast<EditId>(k));
            if (e != scene::kInvalidEntity && scene::World::instance().valid(e)) ents.push_back(e);
        }
        if (ents.empty()) return;
        selectionUi_.multiSel_ = ents;
        multiRebuildSet();
        selectionUi_.sel_ = SelectionState::kSelScene;
        selectionUi_.selEntity_ = ents.front();
    };
    host.push = [this](editor::dl::Edit e) { pushDataLayerEdit(std::move(e)); };
    host.refreshVisibility = [this]() { syncDataLayerVisibility(); };
    editor::drawDataLayersPanel(&showDataLayers_, dataLayers_, dataLayersUi_, host, viewState_.dpi_);
}

// The model already applied `e`; this records it for undo and refreshes what the toggles show.
void SandboxApp::pushDataLayerEdit(editor::dl::Edit e) {
    if (e.empty()) return;
    EditCmd c;
    c.kind = EditCmd::Kind::DataLayers;
    c.label = "Data Layers";
    c.dataLayers = std::move(e);
    pushEdit(std::move(c));
    syncDataLayerVisibility();
}

void SandboxApp::applyDataLayerEdit(const editor::dl::Edit& e, bool undoing) {
    dataLayers_.apply(e, undoing);
    if (!dataLayers_.hasLayer(dataLayersUi_.selected)) dataLayersUi_.selected.clear();
    syncDataLayerVisibility();
}

// Hides the entities of every layer the panel has switched off and restores the ones it hid before. An entity the user H-hid is not ours to restore.
void SandboxApp::syncDataLayerVisibility() {
    scene::World& w = scene::World::instance();
    std::vector<u32> release;
    for (const u32 id : dataLayerHidden_) {
        const scene::Entity e = static_cast<scene::Entity>(id);
        const EditId edit = [&]() { const auto it = entityToEdit_.find(id); return it == entityToEdit_.end() ? EditId(0) : it->second; }();
        if (!w.valid(e) || edit == 0 || !dataLayers_.entityHidden(edit)) release.push_back(id);
    }
    for (const u32 id : release) {
        dataLayerHidden_.erase(id);
        const scene::Entity e = static_cast<scene::Entity>(id);
        if (!w.valid(e)) continue;
        if (const auto it = std::find(selectionUi_.editorHidden_.begin(), selectionUi_.editorHidden_.end(), e); it != selectionUi_.editorHidden_.end()) selectionUi_.editorHidden_.erase(it);
        if (auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) mr->flags |= scene::kMeshRendererVisible;
    }
    for (const editor::dl::Key key : dataLayers_.allMembers()) {
        if (!dataLayers_.entityHidden(key)) continue;
        const scene::Entity e = entityForEdit(static_cast<EditId>(key));
        if (e == scene::kInvalidEntity || !w.valid(e) || dataLayerHidden_.count(static_cast<u32>(e))) continue;
        auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        if (!mr || !(mr->flags & scene::kMeshRendererVisible)) continue;   // already hidden by someone else or not drawable
        mr->flags &= ~scene::kMeshRendererVisible;
        selectionUi_.editorHidden_.push_back(e);
        dataLayerHidden_.insert(static_cast<u32>(e));
    }
}

// A level opened: the DATALAYER records, then each placement's membership (the loader calls dataLayers_.setMembers per entity).
void SandboxApp::loadDataLayers(const fmt::OcWorldData& w) {
    dataLayers_.load(w.dataLayers);
    dataLayerHidden_.clear();
    dataLayersUi_ = editor::DataLayersUi{};
}

void SandboxApp::clearDataLayers() {
    dataLayers_.clear();
    dataLayerHidden_.clear();
    dataLayersUi_ = editor::DataLayersUi{};
}

}  // namespace aver

#endif
