#pragma once
// The editor half of strand hair (docs/HAIR.md "The editor"), two things in one pair of files because the hair area owns exactly these:
//
//   1. THE GROOM TAB. An .ochair opened as an asset (HairGroomEditor, an AssetEditor like FoliageTypeEditor): the scalp, the generator, the simulation
//      materials, the looks, the level of detail and the collision proxies as widgets over the records of the groom, a live preview of the roots and
//      guide strands on the scalp (a 2D projection drawn with the ImGui draw list, no device), "Import curves" for a glTF with LINES, and the budget
//      and advice lines. Undo is a snapshot of the records only: the stored strands and the embedded scalp can be megabytes and sit beside the undo
//      stack, so importing or clearing strands is not an undo step (the tooltips say so). The rules are HairDetailsModel.hpp's (ImGui-free, tested).
//      THE HOOK INTO THE EDITOR IS TWO LINES (shared edits): #include "HairGroomTab.hpp" and, appended after the other factories in SandboxInit.cpp,
//        assetEditors_.registerFactory(&editor::makeHairGroomEditor);   // .ochair
//      plus the Content Browser's icon row for ".ochair" and a "New Hair Groom" entry that writes hairStarterGroom().
//   2. THE HAIR DETAILS SECTION of a placed CHair (drawHairSection), reached from SandboxComponentDetails.cpp through one call after filling a
//      HairSectionEnv.
//
// Both live in their own translation unit because SandboxComponentDetails.cpp is at the Debug COFF section cap. The headless build (tests/hair's
// HairGroomEditorTest, which leaves AVER_WITH_IMGUI undefined) still gets load, save, dirty, undo and every edit; only the widgets are absent.
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

// What the Details section needs from the host beyond the live world, as callbacks (each a thin wrapper over what the other component sections already use in
// SandboxComponentDetails.cpp).
struct HairSectionEnv {
    bool playing = false;
    // Run `mutate` as ONE undo step when `changed` is true (SandboxApp's instantEdit / gestureEdit for the section's kind).
    std::function<void(bool changed, const std::function<void()>& mutate)> instantEdit;
    std::function<void(bool changed, const std::function<void()>& mutate)> gestureEdit;
    // The "Groom (.ochair)" asset row: draws the picker for the component's asset id (key "asset") and applies a change through the applier.
    std::function<void(const char* key, const char* label, u64 assetId)> assetRow;
    std::function<void(const std::string&)> setStatus;
    // Whether a GPU compute host is installed (HairScene::ready()); false means Jolt's CPU backend.
    bool computeHost = true;
};

#if AVER_WITH_IMGUI && AVER_MODULE_HAIR_SCENE
// The Hair section for entity `e` (nothing when it has no CHair). `worldPtr` is the live scene::World.
void drawHairSection(const HairSectionEnv& env, void* worldPtr, u32 entity);
#endif

// What "New Hair Groom" writes (hairmodel::starterGroom): valid as it stands, so the tab opens it.
inline fmt::OcHairData hairStarterGroom() { return hairmodel::starterGroom(); }

// Writes the starter groom to `absPath` as an .ochair. Refuses an existing file (a new asset never overwrites). False with `why`.
bool writeStarterHairGroom(const std::string& absPath, std::string* why);

// ---- the Groom tab ---------------------------------------------------------------------------------------------------------------------------

class HairGroomEditor final : public AssetEditor {
public:
    explicit HairGroomEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // ---- reachable for a headless test ----
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    // The records (no strands, no embedded scalp: those sit beside them); assembled() is the whole groom as save() would write it.
    const fmt::OcHairData& records() const { return state_; }
    fmt::OcHairData assembled() const;
    void markDirty() { dirty_ = true; }

    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Every edit goes through this: one undo step, `mutate` on the records, then sanitize (the clamps that keep valid() true), dirty, preview stale.
    void edit(const std::function<void(fmt::OcHairData&)>& mutate);
    // The same, as part of a gesture already open (a drag): no new undo step.
    void editInGesture(const std::function<void(fmt::OcHairData&)>& mutate);

    // Material and look index lists: add a record with the next unused index (refused at 255), remove one by index (the last of a kind stays).
    bool addMaterial();
    bool removeMaterial(u32 index);
    bool addLook();
    bool removeLook(u32 index);
    bool addProxy(const fmt::OcHairProxy& p);
    bool removeProxy(usize i);
    // Replaces the proxies with the humanoid starter set (the library's rules). False when Aver.Hair is absent from this build.
    bool useStarterProxies();
    // Makes the groom generated again: drops the stored strands (the embedded scalp stays) and turns GENERATE on. Not an undo step.
    void clearStoredStrands();
    // Replaces the stored strands with the curves of a glTF (LINES / LINE_STRIP primitives), as `AverAssetC hair` does. Not an undo step. False with `why`.
    bool importCurves(const std::string& gltfPath, std::string* why);

    // The preview as last built (stale until preview() is asked again after an edit).
    const hairmodel::GroomPreview& preview();
    hairmodel::GroomBudget budget() const { return hairmodel::groomBudgetOf(state_, strands_.strandCount(), strands_.pointCount()); }
    // The advice lines for the groom as it stands (recomputed when the records change).
    const std::vector<std::string>& advice();
    usize storedStrandCount() const { return strands_.strandCount(); }

private:
    void loadFromDisk();
    void syncBuffers();
    void rebuildPreview();

    std::string path_;
    fmt::OcHairData state_;           // everything but the strands and the embedded scalp
    fmt::OcHairStrands strands_;      // STRN, held beside the records
    bool hasScalp_ = false;
    fmt::OcHairScalp scalp_;          // SCLP, held beside the records
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    bool gestureOpen_ = false;

    SnapshotUndo<fmt::OcHairData> history_;

    // The preview, rebuilt when the records change (at most every ~120 ms while a drag runs: previewStale_ plus previewNextMs_).
    hairmodel::GroomPreview preview_;
    bool previewStale_ = true;
    f64 previewNextMs_ = 0.0;
    std::string previewScalpKey_;     // the scalp the cached mesh was read for
    bool previewHasScalp_ = false;
    std::string previewScalpError_;
    std::vector<std::string> advice_;
    bool adviceStale_ = true;
#if AVER_MODULE_HAIR
    hair::ScalpMesh previewScalp_;
#endif

    // Edit scratch for the widgets (text fields, the selected record of each list), plain so they need no #if.
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
    // Each takes the current value from the records and writes through `set` inside edit() / editInGesture(), so a drag is one undo step.
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

// Creates the Groom tab for an .ochair, else nullptr.
std::unique_ptr<AssetEditor> makeHairGroomEditor(const std::string& path);

} // namespace aver::editor
