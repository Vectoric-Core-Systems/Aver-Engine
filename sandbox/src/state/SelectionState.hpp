// Selection and the outliner: primary/multi selection, marquee, outliner tree caches, rename/folder state.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct SelectionState {
#if AVER_MODULE_SCENE
    // ---- MULTI-SELECTION ----
    // Anchor plus set: anchor (sel_/selEntity_) kept as-is, multiSel_ is the rest.
    // INVARIANT: multiSel_ always contains selEntity_ when selected.
    std::vector<scene::Entity> multiSel_;
    // Parallel set for fast O(1) lookup; vector maintains order.
    std::unordered_set<scene::Entity> multiSet_;
    // Outliner row order (multiRange, selectAllInOutliner).
    std::vector<scene::Entity> outlinerOrder_;
    std::string outlinerFilter_;   // name filter box; empty = show everything
    scene::Entity outlinerRenaming_ = scene::kInvalidEntity;
    // The row whose right-click menu was open last frame. The flat Outliner only submits the rows
    // its clipper hands out, and a popup lives inside its row's BeginPopupContextItem, so the clipper
    // pins this row (like the renaming and dragged ones) or scrolling it away would strand the menu.
    scene::Entity outlinerCtxRow_ = scene::kInvalidEntity;
    bool outlinerRenameFocus_ = false;
    char outlinerRenameBuf_[128] = {0};
    std::vector<scene::Entity> editorHidden_;
    // Left-drag on empty space draws marquee; release selects intersecting entities (Ctrl adds).
    bool nudgeEditOpen_ = false;
    bool marqueeArmed_ = false;
    bool marqueeActive_ = false;
    f32 marqueeX0_ = 0.0f, marqueeY0_ = 0.0f, marqueeX1_ = 0.0f, marqueeY1_ = 0.0f;
#endif
#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
    // One Outliner row, kept across frames (see outlinerSignature for cache validity).
    struct OutlinerRow { scene::Entity ent; scene::Entity par; std::string shown; };
    // ---- eye, lock and folders (editor-only; see OutlinerModel.hpp) ----
    std::unordered_set<u32> outlinerLocked_;                  // entities viewport picking skips
    std::unordered_map<u32, std::string> outlinerFolderOf_;   // root entity -> folder path
    std::vector<std::string> outlinerFolders_;                // sorted folder paths, empty ones included
    u32 outlinerFolderRev_ = 0;                               // bumps when folders change, so the row cache rebuilds
    u32 outlinerTypeMask_ = editor::kOutlinerAllTypes;        // type filter
    std::unordered_map<std::string, std::vector<const OutlinerRow*>> outlinerFolderRows_;   // root rows per folder
    std::string outlinerFolderRenaming_;                      // folder being renamed inline
    char outlinerFolderRenameBuf_[96] = {};
    bool outlinerFolderRenameFocus_ = false;
    // Outliner row cache and signature tracking for efficient reuse.
    std::vector<OutlinerRow> outlinerRows_;
    std::unordered_map<u32, std::vector<const OutlinerRow*>> outlinerChildren_;
    std::vector<const OutlinerRow*> outlinerRoots_;
    // Entity of each root and indices of roots that have listed children.
    std::vector<scene::Entity> outlinerRootEnts_;
    std::vector<u32> outlinerParentRoots_;
    // Signature the cache was built from, and whether it has been built.
    u64  outlinerCacheSig_ = 0;
    bool outlinerCacheValid_ = false;
    // True when cache holds no parent/child pair (flat import: every row is a uniform leaf).
    bool outlinerFlat_ = false;
    // One fingerprint per dense slot and next audit cursor position.
    std::vector<u64> outlinerSlotSigs_;
    u32  outlinerAuditCursor_ = 0;
    // outlinerEditMark as of last refresh.
    u64  outlinerEditMark_ = 0;
    // ImGui::GetTime of last rebuild and whether Play is running.
    double outlinerRebuiltAt_ = 0.0;
#endif
#endif
    // Selection: sel_ >= 0 is objects_ index, -1 is nothing, -2/-3/-4 sun/sky/post, kSelScene is entity.
    static constexpr int kSelScene = -5;
    static constexpr int kSelPostVolume = -6;   // the Post Process Volumes entry (levelHeader_.postVolumes)
    int sel_ = 1;
    // AvId (generic "nothing selected" sentinel).
    AvId selEntity_ = kInvalidId;
    // Latched during the scene pass so the outline draws after every surface is down.
    Mat4 selectionOutline_{}; rhi::MeshHandle selectionMesh_ = 0; bool hasSelection_ = false;
    // Every selected entity's (world transform, mesh id) for this frame's outline pass. Rebuilt and cleared right after drawing.
    std::vector<std::pair<Mat4, u64>> selectionOutlines_;
#if AVER_MODULE_SCENE
    scene::Entity outlinerDeleteRequest_ = scene::kInvalidEntity;
#endif
#if AVER_MODULE_PBR
    // Mesh id -> cached outline line mesh (0 = no outline).
    std::unordered_map<u64, rhi::LineHandle> selOutlineLines_;
#endif
    // Selected mesh asset id (distinct from GPU handle). Cleared by placeholder Floor/Cube loop.
    u64 selectionMeshId_ = 0;
};
