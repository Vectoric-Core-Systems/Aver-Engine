#pragma once
// The Face Rig editor tab: an .ocface opened as an asset. Left, the rig's poses (add, duplicate, delete, import the ARKit
// defaults); middle, the shared skinned/morphing preview of the mesh the rig names with the blend-shape sliders under it;
// right, the selected pose's targets and joint deltas, and a Visemes tab (hold a button to preview a mouth shape, type
// "HH AH L OW" to watch it spoken, validate the rig against the mesh, bake a lip-sync track to a clip).
// Design: docs/plans/wave9/MORPH_TARGETS.md 2.10; guide: docs/ANIM_MORPH_FACE.md.
//
// THE HOOK INTO SandboxApp.cpp is the factory line beside the other animation editors:
//   assetEditors_.registerFactory(&editor::makeFaceRigEditor);   // .ocface
// plus the files in sandbox/CMakeLists.txt (SkinPreview.cpp MorphPanel.cpp FaceRigEditor.cpp).
//
// The pose edits are FREE FUNCTIONS, so a headless test reaches them (see BtEditor.hpp for the argument).
#include "AssetEditor.hpp"
#include "MorphPanel.hpp"
#include "SnapshotUndo.hpp"

#include "aver/anim/FaceRig.hpp"
#include "aver/anim/Viseme.hpp"
#include "aver/formats/OcAnim.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// A pose name not already used: `base`, then `base 2`, `base 3`...
std::string faceRigUniquePoseName(const anim::FaceRigAsset& a, const std::string& base);
// Appends an empty pose (its name made unique); returns its index.
i32 faceRigAddPose(anim::FaceRigAsset& a, const std::string& name);
// Copies a pose under a new unique name; returns the copy's index or -1.
i32 faceRigDuplicatePose(anim::FaceRigAsset& a, i32 index);
bool faceRigRemovePose(anim::FaceRigAsset& a, i32 index);
// False for an empty or taken name.
bool faceRigRenamePose(anim::FaceRigAsset& a, i32 index, const std::string& name);
// Adds every pose of the built-in rig the asset does not already have (the 52 ARKit shapes and the 15 visemes), up to the
// 256-pose limit; returns how many were added.
u32 faceRigImportArkit(anim::FaceRigAsset& a);
// Target names the rig's poses use that the mesh lacks (each once, in first-use order). A pose name that is itself a
// mesh target is fine; this lists TARGETS only.
std::vector<std::string> faceRigMissingTargets(const anim::FaceRigAsset& a, const std::vector<std::string>& meshTargets);

class FaceRigEditor final : public AssetEditor {
public:
    explicit FaceRigEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;
    bool usesSharedPreview() const override { return true; }
    ~FaceRigEditor() override;

    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const anim::FaceRigAsset& asset() const { return asset_; }
    i32 selectedPose() const { return sel_; }
    void selectPose(i32 i) { sel_ = i; }

    void pushUndo() { history_.push(asset_); }
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    void markDirty() { dirty_ = true; ++rev_; }

    // Each pushes one undo entry; the ImGui layer calls these, and so do tests.
    i32 addPose(const std::string& name);
    i32 duplicatePose(i32 index);
    bool removePose(i32 index);
    bool renamePose(i32 index, const std::string& name);
    u32 importArkit();

private:
    void loadFromDisk();
    std::string resolveContent(const std::string& rel) const;

    std::string path_;
    anim::FaceRigAsset asset_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    SnapshotUndo<anim::FaceRigAsset> history_;
    i32 sel_ = -1;
    u32 rev_ = 0;            // bumped by every edit, so the preview rebinds its compiled poses only when the rig changed
    u32 rtRev_ = ~0u;

#if AVER_WITH_IMGUI
    void drawLeft();
    void drawPreview(Engine& e, float width, float height);
    void drawPoseTab();
    void drawVisemeTab();
    void bindPreview(Engine& e);
    void stagePreview(Engine& e);
    void touch(bool changed);
    void playPhonemes(const std::string& text);

    bool gesture_ = false, endGesture_ = false;
    anim::FaceRigAsset before_;
    // The preview.
    std::string meshPath_, skelPath_;
    std::string cachedMeshRel_, cachedSkelRel_;   // the asset hints meshPath_ / skelPath_ were resolved from
    std::unordered_map<u64, u32> targetIndex_;
    fmt::OcSkeleton skel_;
    bool skelOk_ = false, bindTried_ = false, bound_ = false, framed_ = false;
    MorphPanel panel_;
    std::vector<u64> targetIds_;
    anim::FaceRigRuntime rt_;
    std::vector<anim::FaceTargetOut> expand_;
    std::vector<Mat4> skin_;
    anim::Pose pose_;
    f32 poseWeight_ = 1.0f;
    i32 heldViseme_ = -1, heldNext_ = -1;   // heldNext_ is what the Visemes tab saw this frame; the preview reads it next frame
    u32 pendingW_ = 0, pendingH_ = 0;
    f64 resizeDue_ = 0.0;
    // Phoneme playback.
    char phonemeBuf_[256] = "HH AH L OW";
    std::vector<anim::VisemeEvent> speech_;
    f32 speechTime_ = 0.0f;
    bool speaking_ = false;
    f64 lastClock_ = 0.0;
    // Validate / bake.
    std::vector<std::string> missing_;
    bool validated_ = false;
    char trackBuf_[260] = {};
    char outBuf_[260] = {};
    std::string status_;
#endif
};

std::unique_ptr<AssetEditor> makeFaceRigEditor(const std::string& path);

} // namespace aver::editor
