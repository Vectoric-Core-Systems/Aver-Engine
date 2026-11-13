// Bottom drawer: content browser, output log and console state; asset-picker filters; file-icon atlases.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct DrawerState {
    std::string importSrc_, importDst_;
    // ---- PlayerStart: where the player spawns in ----
    // Level format had the answer, unread: OcWorldData's hasSpawn/spawnX/Y/Z/Yaw were parsed/written but consulted nowhere.
    // ONE RECORD, SO ONE MARKER: SPAWN is scalar, not a list (two sources of truth is bad).
    // TRANSIENT ENTITY, never pushed to levelEntities_ (like the drone).
    std::vector<std::string> cbDeleteRefs_;
    std::vector<std::string> cbRenameRefs_;   // the same, for the rename dialog
    // Whether the rename dialog repoints what it found (ON by default; repointing is the usual want).
    bool cbRenameRepoint_ = true;
    bool cbMoveRepoint_ = true;     // the Move dialog's "update references" (folders included)
#if AVER_WITH_IMGUI
    // One suggestion row: display (name+description) and insert (bare name).
    struct ConsoleSuggestion { std::string display; std::string insert; std::string tooltip; };
    // One Content Browser root mount.
    struct CbRoot { const char* label; std::string path; bool engine; };
    // Every selected path in the current folder.
    std::vector<std::string> cbSelection_;
    // The paths the current folder is showing, refreshed every frame.
    std::vector<std::string> cbShownPaths_;
    // Drop onto folder, latched at drop time, answered by Copy/Move prompt a frame later.
    std::vector<std::string> cbMoveSources_;
    std::string              cbMoveDest_;
    bool                     cbWantMoveOrCopy_ = false;
    // Asset sheet tile for engine asset extension, -1 if none (glyph+colour for type identification).
    struct AssetKind { const char* icon; ImU32 tint; const char* label; };
    // True when hay contains needle, ignoring case.
    // ---- Asset picker ----
    // One generic widget over label, id candidates; three fields differ only in sources and id meaning.
    struct AssetChoice { std::string label; u64 id = 0; };
    char assetPickerFilter_[64] = {};
    // Candidate list valid while last used on previous/current frame; stamp tracks map size.
    struct PickerCands {
        std::vector<AssetChoice> list;
        int   lastFrame = -2;
        usize stamp = 0;
        void drop() { std::vector<AssetChoice>().swap(list); lastFrame = -2; }
    };
    PickerCands pickMeshCands_, pickMaterialCands_, pickEffectCands_;
#if AVER_MODULE_SCENE
    PickerCands pickPlacedCands_;   // the Add Component asset rows share one picker: only one popup is open at a time
#endif
#endif
    // The open drawer's CURRENT animated height in pixels, 0 when closed. Published to avoid viewport hint drift.
    f32  drawerPixelH_=0.0f;
    // File > Open Level's own picker: the list is rebuilt when it opens, not per frame.
    bool        openLevelPicker_ = false;
    std::string openLevelByName_;              // --open-level, consumed on the first draw
    std::vector<editor::LevelEntry> openLevelList_;
    int         openLevelSelected_ = -1;
    std::string openLevelError_;
    rhi::TextureHandle fileIconsTexture_=0;     // the Content Browser file-type sprite sheet (4 tiles)
    u64 fileIconsUiId_=0;
    f32 fileIconAspect_=0.74f;                  // measured from the sheet; the literal is only the fallback
    rhi::TextureHandle folderIconsTexture_=0;   // the folder sheet (2 tiles: plain, module)
    u64 folderIconsUiId_=0;
    f32 folderIconAspect_=1.24f;
    rhi::TextureHandle assetIconsTexture_=0;    // the generated asset sheet (anim, skeleton, mesh)
    u64 assetIconsUiId_=0;
    f32 assetIconAspect_=0.74f;
    // The tile counts, named once each to avoid silent shifts from mismatched values.
    static constexpr int kFileIconTiles   = 4;
    static constexpr int kFolderIconTiles = 2;
    static constexpr int kAssetIconTiles  = 4;   // anim, skeleton, mesh, graph
    // Tiles are packed into one int so a cached DirListing entry stays one field.
    static constexpr int kAssetTileBase   = 100;
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, int>> fileIconCache_;
    // A .ocgraph's DOMAIN record, read once and kept against the file's mtime (same cache shape as fileIconCache_, rebuilt with the listing).
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, editor::GraphAssetFamily>>
        graphDomainCache_;
    std::unordered_map<std::string, DirListing> dirCache_;
    // Output log (thread-safe via logMutex_). Graph-print feed has timestamp.
    static constexpr size_t kMaxLogLines = 4000;
    std::deque<LogLine> logLines_;
    usize               logMultiRowCount_ = 0;   // Lines in logLines_ with rows > 1.
    u64                 logFirstSerial_ = 0;     // Serial of logLines_.front(): a line keeps its number while older ones drop off.
    i64                 logSelAnchor_ = -1;      // Output Log selection: the serials of the clicked line and the shift-clicked end
    i64                 logSelEnd_ = -1;         // (inclusive, either order); -1 when nothing is selected.
    struct GraphPrint { std::string text; f64 at; u32 count; };
    std::deque<GraphPrint> graphPrints_;
    bool                logAutoScroll_ = true;
    int                 logLevelFilter_ = 0;      // 0 = all, 1 = Info+, 2 = Warn+.
    // Console own scrollback (REPL transcript, not engine log).
    static constexpr size_t kMaxConsoleLines = 2000;
    std::deque<LogLine>      consoleLines_;
    bool                      consoleAutoScroll_ = true;
    bool                      consoleFocusPending_ = false;   // Armed by toggleDrawer(Console).
    char                      consoleInput_[256] = {};
    std::vector<std::string>  consoleHistory_;                // Most-recent-last.
    int                       consoleHistoryPos_ = -1;         // -1 = not recalling history.
    // Browse Variables tab filter box (separate from Content Browser's cbFilter_).
    char                      consoleBrowseFilter_[128] = {};
    // Content Browser folder (empty = content root) and Import source path.
    std::string         cbSelectedDir_;           // Empty = content root.
    char                importPath_[512] = {};
    bool                cbGallery_ = true;        // Tiles vs list.
    f32                 cbTileSize_ = 88.0f;      // Gallery tile edge (dp).
    int                 thumbDiskCapMB_ = 256;    // Size cap of <project>/Saved/ThumbnailCache (0 = no disk cache).
    std::string         cbSelectedFile_;          // Highlighted entry in file view.
    char                cbFilter_[128] = {};      // Search box (filters folder by name).
    // Search box scope: also look under open folder (cbGatherDeepMatches).
    bool                cbSearchDeep_ = false;
    // Type chips, favourites and collections (ContentCollections.hpp). cbView_ is "" for a folder, kCbFavView or a collection name.
    u32                 cbTypeMask_ = 0;
    editor::CbCollections cbCols_;
    std::string         cbColsFor_;               // project dir cbCols_ was loaded for
    std::string         cbView_;
    char                cbNewColBuf_[64] = {};
    bool                cbWantNewCol_ = false;
    std::string         cbNewColAddPath_;         // file to put in the collection being created, if any
    std::string         cbEngineRoot_;            // Empty in shipped build.
    std::vector<std::string> cbHistory_;
    int                 cbHistoryPos_ = -1;
    // Right-click target (not necessarily selected).
    std::string         cbContextPath_;
    bool                cbContextIsDir_ = false;
    // Deferred popup requests.
    bool                cbWantRename_ = false, cbWantDelete_ = false, cbWantNewFolder_ = false;
    bool                cbWantDuplicate_ = false, cbWantImport_ = false;
    char                cbRenameBuf_[256] = {};
    char                cbNewFolderBuf_[128] = {};
    bool                cbWantNewStringTable_ = false;
    char                cbNewStringTableBuf_[64] = {};
    std::string         cbStatus_;                // Last operation outcome.
    int                 cbIdeChoice_ = -1;        // -1 = IdeIntegration default.
    // Bottom drawers: drawerAnim_ is eased 0..1 slide. drawerShown_ survives retraction.
    Drawer              drawer_ = Drawer::None;
    Drawer              drawerShown_ = Drawer::Content;
    f32                 drawerAnim_ = 0.0f;
    f32                 drawerFrac_ = 0.48f;      // Drawer height (fraction of work area).
    f32                 drawerRate_ = 14.0f;      // Slide easing rate.
    bool                cbDoubleClickEnter_ = true;   // Double-click folder to enter (vs single).
    bool                drawerRaise_ = false;     // Focus on frame it opens.
    // Drawer start path (--drawer content:<sub> or console:<sub>). Applied once at first draw.
    std::string         drawerStartSub_;
};
