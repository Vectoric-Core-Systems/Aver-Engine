#pragma once
// Editor half of strand hair (docs/HAIR.md "The editor"): the Groom tab for .ochair assets and the Hair details section of a CHair.
// Own TU: SandboxComponentDetails.cpp is at the Debug COFF section cap. Headless builds (no AVER_WITH_IMGUI) keep load, save, undo and edits; only widgets are absent.
// Hooked in SandboxInit.cpp: assetEditors_.registerFactory(&editor::makeHairGroomEditor); // .ochair
// Undo snapshots the records only; strands and the scalp are not undoable (importing or clearing strands is not an undo step).
#include "AssetEditor.hpp"
#include "HairDetailsModel.hpp"
#include "SnapshotUndo.hpp"

#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// Host callbacks for the Details section, each a thin wrapper over the component sections' helpers.
struct HairSectionEnv {
    bool playing = false;
    // Runs mutate as ONE undo step when changed is true.
    std::function<void(bool changed, const std::function<void()>& mutate)> instantEdit;
    std::function<void(bool changed, const std::function<void()>& mutate)> gestureEdit;
    std::function<void(const char* key, const char* label, u64 assetId)> assetRow;
    std::function<void(const std::string&)> setStatus;
    // Whether a GPU compute host is installed; false means Jolt's CPU backend.
    bool computeHost = true;
};

#if AVER_WITH_IMGUI && AVER_MODULE_HAIR_SCENE
// Hair section for entity e (nothing without a CHair). worldPtr is the live scene::World.
void drawHairSection(const HairSectionEnv& env, void* worldPtr, u32 entity);
#endif

// Refuses an existing file (a new asset never overwrites). False with why.
bool writeStarterHairGroom(const std::string& absPath, std::string* why);

class HairGroomEditor final : public AssetEditor {
public:
    explicit HairGroomEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable from headless tests.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    // Records only (no strands, no scalp); assembled() is the full groom as save() writes it.
    const fmt::OcHairData& records() const { return state_; }
    fmt::OcHairData assembled() const;
    void markDirty() { dirty_ = true; }

    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // One undo step, mutate on the records, then sanitize, dirty, preview stale.
    void edit(const std::function<void(fmt::OcHairData&)>& mutate);
    // As part of an open gesture (a drag): no new undo step.
    void editInGesture(const std::function<void(fmt::OcHairData&)>& mutate);

    // Add uses the next unused index (refused at 255); removing the last of a kind is refused.
    bool addMaterial();
    bool removeMaterial(u32 index);
    bool addLook();
    bool removeLook(u32 index);
    bool addProxy(const fmt::OcHairProxy& p);
    bool removeProxy(usize i);
    // Replaces the proxies with the humanoid starter set. False when Aver.Hair is absent from this build.
    bool useStarterProxies();
    // Drops stored strands (the scalp stays) and turns GENERATE on. Not an undo step.
    void clearStoredStrands();
    // glTF LINES / LINE_STRIP curves, as AverAssetC hair does. Not an undo step. False with why.
    bool importCurves(const std::string& gltfPath, std::string* why);

    // Stale until preview() is called again after an edit.
    const hairmodel::GroomPreview& preview();
    hairmodel::GroomBudget budget() const { return hairmodel::groomBudgetOf(state_, strands_.strandCount(), strands_.pointCount()); }
    const std::vector<std::string>& advice();
    usize storedStrandCount() const { return strands_.strandCount(); }

private:
    void loadFromDisk();
    void syncBuffers();
    void clampSelection();
    void rebuildPreview();

    std::string path_;
    fmt::OcHairData state_;
    fmt::OcHairStrands strands_;
    bool hasScalp_ = false;
    fmt::OcHairScalp scalp_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    bool gestureOpen_ = false;

    SnapshotUndo<fmt::OcHairData> history_;

    // Rebuilt when records change, at most every ~120 ms during a drag.
    hairmodel::GroomPreview preview_;
    bool previewStale_ = true;
    f64 previewNextMs_ = 0.0;
    std::string previewScalpKey_;
    bool previewHasScalp_ = false;
    std::string previewScalpError_;
    std::vector<std::string> advice_;
    bool adviceStale_ = true;
#if AVER_MODULE_HAIR
    hair::ScalpMesh previewScalp_;
#endif

    // Widget scratch (text fields, selected record per list); plain so no #if is needed.
    char scalpBuf_[256] = {};
    char jointsBuf_[256] = {};
    char maskBuf_[256] = {};
    char importBuf_[256] = {};
    std::string importMessage_;
    int selMaterial_ = 0;
    int selLook_ = 0;
    int selProxy_ = 0;
    int view_ = 0;

#if AVER_WITH_IMGUI
    // Widgets take the current value and write it through set inside edit() / editInGesture(), so a drag is one undo step.
    struct ItemState { bool activated = false, active = false, hovered = false; };
    static ItemState probe();
    void field(bool changed, const ItemState& item, const std::function<void(fmt::OcHairData&)>& mutate);
    void dragFloat(const char* label, f32 value, f32 speed, f32 lo, f32 hi, const char* format, const std::function<void(fmt::OcHairData&, f32)>& set, const char* tip = nullptr);
    void dragInt(const char* label, i32 value, f32 speed, i32 lo, i32 hi, const std::function<void(fmt::OcHairData&, i32)>& set, const char* tip = nullptr);
    void checkbox(const char* label, bool value, const std::function<void(fmt::OcHairData&, bool)>& set, const char* tip = nullptr);
    void gradientRow(const char* label, const fmt::OcHairGradient& g, const std::function<void(fmt::OcHairData&, const fmt::OcHairGradient&)>& set, const char* tip = nullptr);
    void drawToolbar();
    void drawAdvice();
    void drawPreview();
    void drawScalp();
    void drawGenerate();
    void drawSim();
    void drawMaterials();
    void drawLooks();
    void drawLod();
    void drawProxies();
    void drawStrands();
#endif
};

std::unique_ptr<AssetEditor> makeHairGroomEditor(const std::string& path);

} // namespace aver::editor
