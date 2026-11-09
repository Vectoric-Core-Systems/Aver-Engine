#pragma once
// The Physical Animation Profile tab: an .ocphysanim opened as an asset (WAVE2_UE W6 step 8). Left, the skeleton as a tree with each bone's
// region colour and the heat of a previewed hit; right, the regions (order is precedence), the selected region's drive and envelope, the reach and
// life settings, the offline hit preview (the real HitReaction envelope) and the problems list. All rules are PhysAnimEditModel.hpp; this is
// the widgets, the skeleton picker and the file. Hook: assetEditors_.registerFactory(&editor::makePhysAnimEditor); // .ocphysanim
#include "AssetEditor.hpp"
#include "PhysAnimEditModel.hpp"
#include "SnapshotUndo.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// A starter profile: the engine defaults and no region.
ragdoll::PhysAnimProfile physAnimStarter(const std::string& name);

class PhysAnimEditor final : public AssetEditor {
public:
    explicit PhysAnimEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    bool loaded() const { return loaded_; }
    const ragdoll::PhysAnimProfile& profile() const { return profile_; }
    bool skeletonLoaded() const { return skelOk_; }

    // The edits the widgets run (each pushes one undo entry), public so a test can reach them.
    void edit(const std::function<void(ragdoll::PhysAnimProfile&)>& fn);
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    bool setSkeleton(const std::string& skelPath);
    std::vector<std::string> problems() const;

private:
    void loadFromDisk();
    void rescanSkeletons();
    void refreshPreview();

    std::string path_;
    ragdoll::PhysAnimProfile profile_;
    bool loaded_ = false, dirty_ = false;
    std::string loadError_;
    SnapshotUndo<ragdoll::PhysAnimProfile> history_;

    fmt::OcSkeleton skel_;
    bool skelOk_ = false;
    std::string skelPath_;
    std::vector<std::string> skelChoices_;
    bool skelScanned_ = false;

    int selRegion_ = -1;          // -1 = the defaults
    int selBone_ = -1;
    f32 impulse_ = 0.0f;          // 0 = the profile's full impulse
    physanim::HitPreview preview_;
    bool previewStale_ = true;

#if AVER_WITH_IMGUI
    ragdoll::PhysAnimProfile snap_;   // the profile as of the last frame no widget was active: the "before" of a drag
    char renameBuf_[64] = {};
    bool renaming_ = false;
#endif
};

std::unique_ptr<AssetEditor> makePhysAnimEditor(const std::string& path);

}  // namespace aver::editor
