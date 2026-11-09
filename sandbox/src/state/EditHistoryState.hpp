// Undo/redo history: command stacks, entity-to-edit-id maps, the in-flight gesture snapshots and the editor clipboard; EditCmd and its helper types move with it.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct EditHistoryState {
    // The gizmo's world-agnostic view of a selection's transform. Rotation is in Euler degrees.
    struct EditXform { Vec3 pos, rotDeg, scale; };
    // One undoable edit (transform, create/destroy). Components captured via EntitySnapshot.
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj, LandscapeStroke, FoliageStroke, Reparent, Material, Rename, RemoveComponent, Visibility, Collision, Animation, Prefab, LevelInstance, LandscapeWeights, WaterBodies, PlacedComponent, OutlinerMeta, DataLayers };
        Kind kind = Kind::Transform;
        // Monotonic serial for save points; never reused so undo/redo cross saves correctly.
        u64 serial = 0;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};

        // Other entities moved with anchor as one command (avoids "undo partway" inconsistency). Stored as LOCAL transforms.
        // root: the entity had no parent when recorded, so its local is in the scene frame and a floating-origin shift moves it.
        struct AlsoMoved { EditId id = 0; Transform beforeLocal, afterLocal; bool root = true; };
        std::vector<AlsoMoved> alsoMoved;

        // VISIBILITY: before/after per entity, replayed through setAuthoredVisible().
        struct VisibilityChange { EditId id = 0; bool before = true; bool after = true; };
        std::vector<VisibilityChange> visibility;
        // COLLISION: before/after per entity, replayed through setEntityCollide().
        std::vector<VisibilityChange> collide;
        // ANIMATION: before/after per entity (empty clip = none), replayed through applyEntityAnim().
        struct AnimChange { EditId id = 0; EntityAnim before, after; };
        std::vector<AnimChange> animation;
        std::string label;        // outliner display name; editor-owned bookkeeping, not World's
        // OUTLINERMETA: lock and folder changes (editor-only state, never saved with the level; currentEditMark skips them).
        struct OutlinerMetaChange { EditId id = 0; bool lockBefore = false, lockAfter = false; std::string folderBefore, folderAfter; };
        std::vector<OutlinerMetaChange> outlinerMeta;
        std::vector<std::string> foldersBefore, foldersAfter;   // the whole folder list, when foldersChanged
        editor::dl::Edit dataLayers;   // DATALAYERS: the layer list and membership change, replayed through applyDataLayerEdit()
        bool foldersChanged = false;
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;    // scene entity: asset name, persisted id, every other component
#endif
        // Whether to give recreated/pasted entity a body (shape is derived fresh each time).
        bool hadBody = false;
        // Authoring flags (nocollide, snapToGround) not in snap; held in entityCollide_/entitySnapZ_. Fixes destroy-undo bug.
        bool  hadCollide = true;      // the default saveLevel writes for an entity it has no entry for
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        // Non-empty className: Create/Destroy of a graph-class actor, replayed by respawning the class (classSlot indexes
        // classAuthored_), not rebuilt from `snap`.
        std::string className;
        i32 classSlot = -1;
        // Object animation (empty clip = none); not in snap (Play advances its clock).
        EntityAnim hadAnim;
        // Vehicle preset (empty = no car); not in snap, level-held.
        std::string hadVehicle;
        MeshObj objSnapshot{};    // CreateObj/DestroyObj payload; MeshObj is trivially copyable
#if AVER_MODULE_PBR
        // Material payload: whole MaterialDesc (controls interact; replaying one field could restore invalid state). Undo depth bounds cost.
        pbr::MaterialHandle matHandle = 0;
        pbr::MaterialDesc matBefore{}, matAfter{};
#endif
        // Rename payload (display name, not CName).
        std::string renameBefore, renameAfter;

#if AVER_MODULE_SCENE
        // Prefab payload: the prefab state before and after one operation (create, place, apply, revert,
        // unpack, ...). Shared so copying a command around the undo stacks stays cheap.
        std::shared_ptr<editor::PrefabEdit> prefab;
        // [wave3/level-instances] Level instance payload: the instance state before and after one operation.
        std::shared_ptr<editor::LevelInstanceEdit> levelInst;
#endif

#if AVER_MODULE_SCENE
        // Removed component type and byte-exact contents (EntitySnapshot::Comp shape).
        editor::EntitySnapshot::Comp removedComponent;
        // PlacedComponent: one Add Component / Remove Component / Details edit of an animation or AI component, as the
        // COMPONENT record before and after (nullopt = the component is absent). One command covers Add, Edit and Remove.
        std::string pcType;
        std::optional<fmt::OcPlacedComponent> pcBefore, pcAfter;
#endif

        // LandscapeStroke: rect diff, not snapshot (512x512 section = 1 MB, touched rect = KB). One entry per stroke: one rect per
        // section the stroke touched (inclusive sample bounds, before / after).
        std::vector<game::GameLandscape::HeightEdit> landHeights;

        // wave3 LandscapeWeights: per touched section, its stored paint planes over a rect, plane-major, before / after.
        std::vector<game::GameLandscape::WeightEdit> landWeights;
        // wave3 WaterBodies: the level's RIVER / LAKE records before and after one edit gesture (a few records, so whole copies).
        std::vector<fmt::OcRiver> riversBefore, riversAfter;
        std::vector<fmt::OcLake> lakesBefore, lakesAfter;

        // FoliageStroke: every entity one stroke created/erased as one entry (dozens/sec would cost many Ctrl+Z). recreateFrom() undoes erases.
#if AVER_MODULE_SCENE
        std::vector<editor::EntitySnapshot> batchSnaps;
        std::vector<EditId>                 batchIds;
#endif
        bool batchWasErase = false;   // which direction undo has to run

#if AVER_MODULE_SCENE
        // Descendants (parents before children), fixing delete-subtree data loss. parent indexes THIS vector (-1 = root).
        struct DestroyedNode {
            EditId id = 0;
            i32 parent = -1;
            EditXform xf{};
            std::string label;
            editor::EntitySnapshot snap;
            bool hadBody = false;   // whether it had a body
            // Not components, so not in snap.
            bool  hadCollide = true;
            bool  hadSnapZ   = false;
            f32   snapZ      = 0.0f;
            EntityAnim hadAnim;
        };
        std::vector<DestroyedNode> subtree;

        // The destroyed entity's own parent, as an EditId (0 = was a root).
        EditId parentId = 0;

        // Reparent before/after parent (0 = root). Separate from parentId to avoid confusion.
        EditId reparentOldParentId = 0;
        EditId reparentNewParentId = 0;
#endif
    };
#if AVER_MODULE_LANDSCAPE
#if AVER_MODULE_SCENE && AVER_WITH_IMGUI
    EditCmd foliageBatch_{};
#endif
#endif
    // Serial the level was last written at. 0 means "as loaded, unedited".
    u64 savedEditMark_ = 0;
    u64 editSerialNext_ = 0;
    static constexpr std::size_t kUndoDepth = 128;
    std::vector<EditCmd> undoStack_, redoStack_;
    std::unordered_map<EditId, AvId> editToEntity_;
    std::unordered_map<u32, EditId> entityToEdit_;
    EditId nextEditId_ = 1;
    EditXform editBefore_{};
    bool editBeforeValid_ = false;
    // Non-anchor entities' local transforms at drag start (alsoMoved in EditCmd).
    std::vector<std::pair<EditId, Transform>> multiMoveBefore_;
    // Copy/Duplicate source for Paste; entities/hasObject set exclusively by copySelection.
    struct ClipboardEntity {
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;
        // Everything under it too; copying parent alone used to paste childless.
        std::vector<EditCmd::DestroyedNode> subtree;
#endif
        EditXform xform{};
        bool hadBody = false;
        // Root's non-component flags and object animation (children keep theirs in subtree).
        bool  hadCollide = true;
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        EntityAnim hadAnim;
    };
    struct EditorClipboard {
        // Empty means nothing was copied.
        std::vector<ClipboardEntity> entities;

        // Placeholder path single-item (objects_/MeshObj has no multi-selection).
        bool hasObject = false;
        MeshObj object{};
    };
    EditorClipboard clipboard_;
    // The Details panel's Animation drag in flight: every selected entity's EntityAnim from the frame the widget was grabbed.
    std::vector<EditCmd::AnimChange> animEditBefore_;
};
