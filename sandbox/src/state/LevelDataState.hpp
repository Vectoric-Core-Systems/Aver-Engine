// Per-level side tables and caches: entity label/body/anim maps, stream/HLOD bookkeeping, level header copies, per-mesh pick/tri caches.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct LevelDataState {
    // Outliner display names (not scene::World::name which holds asset path).
    std::unordered_map<u32, std::string> entityLabels_;
    std::unordered_map<std::string, int> labelCounts_;
    std::unordered_map<u32, int32_t> entityBodies_;
    // Every PCGVOLUME the loaded level carried, kept verbatim so a save cannot drop them.
    std::vector<fmt::OcPcgVolume> levelPcgVolumes_;
    fmt::OcWorldData levelHeader_;
    // Whether each level entity's placement said `nocollide`. No component for this: load-time instruction, so without it the save forced `collide = true` on everything.
    std::unordered_map<u32, bool> entityCollide_;
    // Each entity's authored object animation; absent = none. The CAnimator on the entity is only the live clock, so saveLevel reads this map.
    std::unordered_map<u32, EntityAnim> entityAnim_;
    // Entities placed with `snap`, and the AUTHORED z offset each was placed at. Absent = not snapped.
    std::unordered_map<u32, f64> entitySnapZ_;
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    // driveAnimatedBodies' list, kept between frames and rebuilt only when it can have changed.
    std::vector<world::AnimatedBody> animatedBodies_;
    bool animatedBodiesBuilt_ = false;
    usize animatedBodiesFromAnim_ = 0;
    usize animatedBodiesFromBodies_ = 0;
    u64 animatedBodiesFromRev_ = 0;
#endif
    // True while the open level loaded via legacy OCMAP path, not OCWORLD. Reset by unloadLevel.
    bool levelIsLegacyOcmap_ = false;
    // Legacy header for OCMAP-only record kinds (ROOT, CLIENT, SURFACE, GROUND, KILLZ).
    fmt::OcMapData legacyMapHeader_;
    // Entity legacy data: deform flag, surface index, material name. See onLegacyOcmapInstantiated.
    std::unordered_map<u32, bool> entityLegacyDeform_;
    std::unordered_map<u32, i32> entityLegacySurface_;
    std::unordered_map<u32, std::string> entityLegacyMaterial_;
#if AVER_MODULE_SCENE
    std::unordered_map<u32, u64> entityPlaceId_;
    std::unordered_map<u32, std::string> entityAuthoredName_;   // an instance member's authored `name`, for a commit
    bool capturingInstance_ = false;                             // fillPlacementFromEntity may read members (a commit)
    scene::Entity instanceSelPinned_ = scene::kInvalidEntity;    // the selected instance stays loaded while it is selected
#if AVER_MODULE_SCENE
    std::string streamRegenReport_;
    std::string hlodReport_;
    bool hlodForce_ = false;
    std::string cookChunksReport_;
    usize streamSeenCount_ = 0;                  // levelEntities_ size at the last adoption pass
    std::unordered_set<u32> streamAdopted_;      // checked by adoptNewLevelEntities (adopted or not a placement)
    std::unordered_set<u32> streamSelPinned_;    // pinned because selected
    bool streamPlayWas_ = false;
    std::unordered_set<u32> streamPlayResident_; // streamed entities resident when Play started
    std::vector<i32> streamOutSlot_;             // record index -> saved placement line, last save
    std::vector<fmt::OcSeqTrack> streamSeqTracks_;   // object tracks of a streamed level, kept aside
    std::vector<std::string> streamSeqOwner_;        // [wave5/sequencer] the sequence (by name) each aside track belongs to
    std::vector<fmt::OcSequence> streamSeqHeaders_;  // the level's sequences without tracks, to rebuild them on save
    // Label each streamed entity got when it streamed in (a changed label is a rename), and what is pinned.
    std::unordered_map<u32, std::string> streamLabel_;
    std::unordered_set<u32> streamPinned_;
#  if AVER_MODULE_VOXI
    // The start level's foliage, kept until the renderer attaches (see afterInstantiate).
    fmt::OcWorldData foliagePendingWorld_;
    std::vector<std::string> foliagePendingTables_;
    bool foliagePending_ = false;
#endif
#endif
    std::vector<scene::Entity> levelEntities_;
#if AVER_MODULE_FRAMEWORK
    using ClassInstance = editor::LevelClassInstance;   // same alias as SandboxApp::ClassInstance
    std::vector<ClassInstance> levelClassInstances_;
    std::vector<fmt::OcWorldPlacement> classAuthored_;      // every class placement of the open level (loaded first, session-added appended)
    std::vector<i64> classAuthoredIndex_;                   // parallel: its placement index in the loaded file, -1 if added this session
    std::vector<editor::GraphClassDecl> classPickList_;
    bool classPickDirty_ = true;                           // rescan when the Add popup opens
#endif
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif
    // Mesh id -> project-relative path. Handles/bounds/parts are in content_.
    std::unordered_map<u64, std::string>     meshPathById_;
    // Entities missing mesh or with invisible bit. Per-entity or per-id fault. Never cleared on reload.
    // Invisible: last generation reported per entity slot (0 = none); generations only rise per slot.
    std::vector<u8> undrawnInvisibleGen_;
    std::unordered_set<u64> undrawnMissingMesh_;
    // Triangle count per mesh id (same key as content_). Enables budget reporting instead of guessing.
    std::unordered_map<u64, u32> meshTris_;
    // Skeletal mesh ids (have skin weights, colored differently in Content Browser).
    std::unordered_set<u64> skinnedMeshIds_;
    // Pick geometry per mesh (lazy-loaded on first click). Empty = tried and unavailable.
    std::unordered_map<u64, aver::editor::PickGeometry> pickGeometry_;
    int lastSceneDrawn_=-1;           // Last scene-entity draw count (startupComplete settle input).
    int lastSceneCulled_=-1;          // Cull count.
    int lastSceneOwnerHidden_=-1;     // Owner-hide count.
#endif
    // Depth proxy map (populated if Trifactor, empty if not). Each lookup answers 0 if no entry.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxy_;
};
