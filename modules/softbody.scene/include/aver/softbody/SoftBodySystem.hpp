// SoftBodySystem: cloth and destruction on scene entities, over Jolt soft bodies (docs/SOFTBODY.md; depth features in docs/plans/wave10/SOFTBODY_DEPTH.md).
// Owns every entity's Jolt soft body: cage, bodies, damage, tears, and the PieceViews Aver.Render.SoftBody draws.
// Attach through attachCloth / attachDestructible / attachSoftField, never world.addComponent (storage is zero-filled, no constructor runs).
// Deterministic per binary (ascending entity, then piece id); cross-platform determinism is not claimed.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/scene/World.hpp"
#include "aver/softbody/Caps.hpp"
#include "aver/softbody/Damage.hpp"
#include "aver/softbody/Fields.hpp"
#include "aver/softbody/Islands.hpp"
#include "aver/softbody/ProxyBind.hpp"
#include "aver/softbody/SimMesh.hpp"
#include "aver/softbody/SoftAsset.hpp"
#include "aver/softbody/SoftState.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::softbody {

// sourceVertex of a draw vertex that is not authored (a cap's back-skin or wall vertex).
inline constexpr u32 kGeneratedVertex = 0xFFFFFFFFu;

inline constexpr u32 kClothNoWorld = 0x1;            // negative sense: zero-filled = collides with the world
inline constexpr u32 kClothNoCharacterProxy = 0x2;
inline constexpr u32 kClothNoWind = 0x4;
inline constexpr u32 kClothTearable = 0x8;
inline constexpr u32 kClothRelease = 0x10;           // script sets, the system clears

inline constexpr u32 kDestructNoDebris = 0x1;
inline constexpr u32 kDestructIndestructible = 0x2;

inline constexpr u32 kStateAwake = 0x1, kStateFrozen = 0x2, kStateReleased = 0x4, kStateAssetStale = 0x8;
inline constexpr u32 kStateOverCap = 0x10;           // welds past the particle cap with no proxy: authored mesh, not simulated
inline constexpr u32 kStateProxy = 0x20;
inline constexpr u32 kStatePieceShift = 8;

struct CCloth {
    u64 asset = 0;                // .ocsoft ObjectId; 0 = preset only
    u32 preset = 0;
    u32 flags = 0;
    f32 windScale = 0;            // 0 reads 1
    f32 stiffnessScale = 0;       // 0 reads 1; scales compliance by 1/s
    f32 reachScale = 0;
    f32 freezeDistanceCm = 0;     // 0 = system default
    f32 outMaxStrain = 0;
    u32 outState = 0;
};

struct CDestructible {
    u64 asset = 0;
    u32 preset = 0;
    u32 flags = 0;
    f32 toughnessScale = 0;       // 0 reads 1; multiplies toughnessJ and the strain limits
    f32 impactScale = 0;          // 0 reads 1; multiplies deposited energy
    f32 debrisLifetimeS = 0;      // 0 = asset default (also maxPieces)
    u32 maxPieces = 0;
    f32 outDamage = 0;            // broken beams / total, 0..1
    u32 outBroken = 0, outPieces = 0, outState = 0;
};
static_assert(sizeof(CCloth) == 40 && sizeof(CDestructible) == 48, "the soft-body components must be padding-free");

inline constexpr u32 kFieldDisabled = 0x1;   // negative sense: zero-filled = enabled
inline constexpr u32 kFieldOnce = 0x2;       // apply once as a pulse, then set kFieldDisabled

inline constexpr u32 kFieldStateActive = 0x1, kFieldStateFired = 0x2;

// A placed field: the entity's world matrix is the shape frame. Continuous (strength in cm/s per second, energy in J/s) unless kFieldOnce.
struct CSoftField {
    u32 kind = 0, shape = 0, flags = 0, targets = 0;      // targets 0 reads kFieldTargetAll, except Kill which reads Debris only
    f32 extentX = 0, extentY = 0, extentZ = 0;            // 0 reads 100
    f32 strength = 0, energyJPerS = 0, falloff = 0;       // falloff 0 reads 1
    f32 dirX = 0, dirY = 0, dirZ = 0;                     // all zero reads +Z for DirectionalForce
    f32 turbulence = 0;
    u32 outTouched = 0;
    u32 outState = 0;                 // bit0 active, bit1 fired (a kFieldOnce field after it ran)
};
static_assert(sizeof(CSoftField) == 64, "CSoftField must be padding-free");

struct Options {
    u32 maxLivePieces = 48;
    u32 maxActiveParticles = 20000;
    u32 maxRebuildsPerFrame = 2;
    u32 maxDrawBuildsPerFrame = 8;
    u32 maxDepositsPerFrame = 64;
    u32 minPieceTriangles = 4;
    f32 debrisLifetimeS = 12.0f;
    f32 debrisSleepS = 2.0f;
    f32 despawnShrinkS = 0.6f;
    f32 freezeDistanceClothCm = 6000.0f;
    f32 freezeDistanceDebrisCm = 4000.0f;
    i32 clothLayer = 14;
    i32 proxyLayer = 15;
    u32 damageEvalEveryNSteps = 2;      // in fixed steps
    f32 lodNearCm = 1500.0f;            // full iterations inside; x0.5 to lodMidCm; x0.25 beyond (min 2)
    f32 lodMidCm = 3500.0f;
    f32 teleportCm = 150.0f;            // a root moved farther than this in one frame hard-skins
    f32 hotSeconds = 2.0f;              // a piece is hot this long after an impact or deposit, or while its max strain exceeds watchStrain
    f32 watchStrain = 0.005f;
    bool capsEnabled = true;            // master switch for thick breaks (an asset's caps.mode still applies)
    bool bakeEnabled = true;
    f32 bakeAfterS = 3.0f;
    f32 cullPixelRatio = 0.004f;        // debris and cap entities below this radius-over-distance are not drawn
    u32 fxMaxPerTick = 16;
    u32 fxSoundsPerTick = 6;
    f32 fxMaxDistanceCm = 6000.0f;      // effects farther than this from the view are dropped
    u32 restoreMaxReplayGroups = 64;    // saved tear-log groups beyond this are merged on load
};

struct Budget {
    u32 maxPieces = 48;
    u32 maxParticles = 20000;
    u32 maxRebuilds = 2;
    f32 debrisLifetimeS = 12.0f;
};

enum class SoftEventKind : u8 { Impact, Torn, Detached, Dust, Repaired, Released };

struct SoftEvent {
    SoftEventKind kind = SoftEventKind::Impact;
    u32 entity = 0;
    u32 piece = 0;
    u32 beam = 0;
    Vec3 point{0, 0, 0};
    f32 value = 0.0f;
    u32 seq = 0;
};

// What the draw side needs for one live piece. Pointers stay valid until the next tick().
struct PieceView {
    scene::Entity entity = scene::kInvalidEntity;   // draws it: the root, a debris entity or a cap entity
    u32 piece = 0;
    u32 epoch = 0;                    // changes when the draw mesh must be rebuilt (tear, release, cap change, bake)
    u32 vertexCount = 0;
    const Vec3* world = nullptr;
    const u32* indices = nullptr;
    u32 indexCount = 0;
    const u32* sourceVertex = nullptr;   // draw vertex -> authored vertex (UV and rest data), or kGeneratedVertex
    u64 meshId = 0;
    Vec3 origin{0, 0, 0};             // debris: pack relative to this
    Vec3 centroid{0, 0, 0};
    bool useEntityWorld = true;       // root piece: pack through the entity's CWorld inverse
    f32 scale01 = 1.0f;
    bool frozen = false;
    bool empty = false;
    const f32* uv = nullptr;          // null means all authored; read for kGeneratedVertex entries
    u32 layer = 0;                    // 0 surface (with merged caps), 1 the interior-material cap entity
    bool revealOnReady = false;       // debris and cap entities start invisible: show once this view has a resident
    bool bake = false;                // settled: one plain static mesh from these vertices, never packed again
};

struct Stats {
    u32 entries = 0, pieces = 0, debris = 0;
    u32 activeParticles = 0, awakePieces = 0, frozenPieces = 0;
    u32 rebuildsLastTick = 0, depositsLastTick = 0, eventsPending = 0;
    u32 bakedPieces = 0, heldPieces = 0, fieldsApplied = 0, transientFields = 0;
    u32 fxSent = 0, fxDropped = 0;
};

struct SoftBodyState {
    bool found = false;
    f32 damage01 = 0.0f, maxStrain = 0.0f;
    u32 pieces = 0, brokenEdges = 0;
    bool awake = false, frozen = false;
};

// The last damage deposit on an entity, for the Tuning HUD (docs/SOFTBODY.md "Tuning").
struct HitInfo {
    bool valid = false;
    u8 kind = 0;
    f32 closingCmS = 0.0f;            // an impact's closing speed, else 0
    f32 energyJ = 0.0f, radiusCm = 0.0f;
    u32 beamsDamaged = 0, broken = 0;
    f32 maxStrain = 0.0f;
    f32 maxSetCm = 0.0f;              // the largest permanent set now, cm
    u32 tick = 0;
    u32 pieces = 0, baked = 0;
};

struct DentInfo {
    bool found = false;
    u32 dentedEdges = 0;
    f32 maxSetCm = 0.0f;
    u32 debrisPieces = 0;
};

// One break effect to dispatch (docs/SOFTBODY.md 2.5). `particle` and `sound` are fnv1a64 of the content-relative path, 0 = none.
struct FxRequest {
    FxSlot slot = FxSlot::Impact;
    scene::Entity entity = scene::kInvalidEntity;
    Vec3 pos{0, 0, 0}, dir{0, 0, 1};
    u32 count = 0;
    u64 particle = 0;
    u64 sound = 0;
    f32 volume = 1.0f, pitch = 1.0f, innerCm = 300.0f, outerCm = 4000.0f;
};
// Called at the end of the tick for requests that survive the budgets.
using SoundFn = void (*)(const FxRequest& r, void* user);

// Host seams, installed through the setters below.
// u64 meshId -> decoded mesh (host content cache); null when it does not resolve yet.
using MeshSourceFn = const fmt::OcMeshData* (*)(u64 meshId, void* user);
// u64 asset id -> parsed .ocsoft; null when absent.
using AssetSourceFn = const SoftAsset* (*)(u64 assetId, void* user);
using PaletteFn = const Mat4* (*)(scene::Entity e, u32& count, void* user);
using ProxyFn = void (*)(scene::Entity e, bool on, void* user);
// The graph notify seam: (entity, "SoftBodyTorn", user).
using NotifyFn = void (*)(scene::Entity e, const char* name, void* user);
using BoneIndexFn = i32 (*)(scene::Entity e, const std::string& bone, void* user);
using PathFn = std::string (*)(u64 objectId, void* user);
// The physics handle of the ragdoll that belongs to a skeleton entity (the group id a soft body can ignore), or 0.
using RagdollGroupFn = i32 (*)(scene::Entity skeleton, void* user);
// Appends the extra PieceViews of another system (SoftShapeSystem: jelly and rope); see addViewSource.
using ViewSourceFn = void (*)(std::vector<PieceView>& out, void* user);

class SoftBodySystem {
public:
    SoftBodySystem();
    ~SoftBodySystem();
    SoftBodySystem(const SoftBodySystem&) = delete;
    SoftBodySystem& operator=(const SoftBodySystem&) = delete;

    // Registers CCloth, CDestructible and CSoftField (idempotent). CSoftBody (id 13) stays the draw marker.
    static u32 registerComponents(scene::World& world);
    // Defaults over the zero-filled bytes; adds CSoftBody if absent. Null: mesh-less renderer, invalid entity, or the other kind already present.
    static CCloth* attachCloth(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset = 0);
    static CDestructible* attachDestructible(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset = 0);
    // Radial sphere of 100 cm, all targets; the entity's transform is the frame.
    static CSoftField* attachSoftField(scene::World& world, scene::Entity e);
    static u32 clothType();
    static u32 destructibleType();
    static u32 softFieldType();

    void install(scene::World& world);
    void uninstall();

    void setMeshSource(MeshSourceFn fn, void* user) { meshFn_ = fn; meshUser_ = user; }
    void setAssetSource(AssetSourceFn fn, void* user) { assetFn_ = fn; assetUser_ = user; }
    // Installs the default mesh and asset sources: decode caches over this resolver.
    void setPathResolver(PathFn fn, void* user);
    void setPaletteSource(PaletteFn fn, void* user) { paletteFn_ = fn; paletteUser_ = user; paletteSet_ = true; }
    void setCharacterProxyFn(ProxyFn fn, void* user) { proxyFn_ = fn; proxyUser_ = user; }
    void setNotifySink(NotifyFn fn, void* user) { notifyFn_ = fn; notifyUser_ = user; }
    void setBoneIndexFn(BoneIndexFn fn, void* user) { boneFn_ = fn; boneUser_ = user; }
    // A cloth with "group": { "ragdollOf": "owner" } ignores that ragdoll. Null: the group is never applied.
    void setRagdollGroupFn(RagdollGroupFn fn, void* user) { ragdollGroupFn_ = fn; ragdollGroupUser_ = user; }
    void setViewPos(const Vec3& p) { viewPos_ = p; }
    Vec3 viewPos() const { return viewPos_; }
    // Sound requests go to the host adapter (Runtime/include/aver/game/SoftBodyEffects.hpp). Null: silent.
    void setSoundSink(SoundFn fn, void* user) { soundFn_ = fn; soundUser_ = user; }
    void setEffectsEnabled(bool on) { effectsOn_ = on; }
    bool effectsEnabled() const { return effectsOn_; }

    // Once per frame after the physics step and animSystem().tick, before the draw scene updates.
    void tick(scene::World& world, f32 dt);
    // Destroys bodies, debris and entries; keeps options and wind. Also drops transient fields, queued pulses, the effect queue and pending restore blobs.
    void clear();

    // Inbox ops are applied in arrival order at the next tick.
    bool damage(scene::Entity e, Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind);
    u32 explode(Vec3 centre, f32 radiusCm, f32 impulseCmS, f32 energyJ);
    // Damage to every entity with a piece in reach (the framework relay, entity 0). Returns how many.
    u32 damageNear(Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind);
    u32 cutPreview(scene::Entity e, Vec3 point, f32 radiusCm) const;
    u32 tearAt(scene::Entity e, Vec3 point, f32 radiusCm);
    bool repair(scene::Entity e);
    bool release(scene::Entity e);
    bool setPreset(scene::Entity e, u32 preset);
    void setWind(Vec3 v, f32 gust, f32 gustHz, f32 turbulence);
    void setBudget(const Budget& b);
    bool setAsleep(scene::Entity e, bool asleep);
    // Destroys the entity's debris now (the shrink is skipped).
    bool clearDebris(scene::Entity e);
    u32 repairAll();
    Options& options() { return opt_; }
    const Options& options() const { return opt_; }

    // One application at the next tick, in arrival order. Returns the pieces in reach now (an upper bound of those touched).
    u32 pulseField(const FieldSpec& f);
    // Transient continuous field (perSecond forced on) for lifetimeS seconds. Returns a handle; 0 = refused.
    u32 startField(const FieldSpec& f, f32 lifetimeS);
    bool stopField(u32 handle);
    // Damage pulse in a sphere plus a directional kick along dir (zero = radial): the crash helper. Returns the pieces in reach now.
    u32 impact(Vec3 point, Vec3 dir, f32 radiusCm, f32 energyJ, f32 kickCmS);
    // An entity with a CSoftField that destroys itself after lifetimeS (move it to move the field).
    scene::Entity spawnFieldEntity(scene::World& world, const FieldSpec& f, f32 lifetimeS);
    // A save skips these: their lifetime is not saved.
    bool isTransientFieldEntity(scene::Entity e) const { return spawnedLife_.count(e) != 0; }

    // Serialises the entity's damage as the `softbody` channel payload. False (and `out` untouched) when it has none.
    bool saveState(scene::Entity e, std::string& out) const;
    // Stores a payload for the (new) entity; applied when its entry is built. False for a malformed payload.
    bool restoreState(scene::Entity e, std::string_view payload);

    // kPa at rest volume, applied at the next tick. False for no cloth body, a destructible or a negative value; 0 deflates.
    bool setPressure(scene::Entity e, f32 kPa);
    f32 pressureOf(scene::Entity e) const;
    // Physics handle of the root piece; 0 when the entity has no live soft body.
    i32 bodyOf(scene::Entity e) const;
    scene::Entity entityOfBody(i32 body) const;
    // fn runs at the end of every publish() and appends to pieces(), so the draw side reads one list. refreshViewSources() rebuilds that tail after the other system ticks.
    void addViewSource(ViewSourceFn fn, void* user);
    void removeViewSource(ViewSourceFn fn, void* user);
    void refreshViewSources();
    // A mesh with no file (the rope tube's placeholder cube); meshData(id) returns it before any host source.
    void registerSyntheticMesh(u64 id, const fmt::OcMeshData& mesh);
    const SoftAsset* softAsset(u64 id) { return assetFor(id); }
    void notifyEntity(scene::Entity e, const char* name) {
        if (notifyFn_) notifyFn_(e, name, notifyUser_);
    }

    const PieceView* pieces(u32& count) const;
    u32 drainEvents(SoftEvent* out, u32 max);
    Stats stats() const { return stats_; }
    SoftBodyState state(scene::Entity e) const;
    HitInfo lastHit(scene::Entity e) const;
    DentInfo dents(scene::Entity e) const;
    // The draw side reads UVs from it.
    const fmt::OcMeshData* meshData(u64 meshId);
    u32 pieceCount(scene::Entity e) const;
    // Strain debug overlay: the entity's cage, or null. Valid until the next tick.
    const Cage* cageOf(scene::Entity e) const;
    Vec3 windVelocity() const { return wind_; }
    // Floating-origin shift (LARGE_WORLD 2.2.5): subtracts deltaCm from every cached world position (entry lastWorld, cage, piece draw data, hit points, field frames).
    // Call after World::shiftRoots and the physics shift.
    void rebase(const Vec3& deltaCm);
    struct FieldView { u32 handle = 0; FieldSpec spec; f32 remainingS = 0.0f; };
    // Field gizmos: the live transient fields, and a component field's spec as the step applies it.
    std::vector<FieldView> transientFields() const;
    static bool specOfComponent(scene::World& world, scene::Entity e, FieldSpec& out);

private:
    // One body of an entry: the root, or a detached island. Owns a subset of the entry's cage.
    struct Piece {
        u32 id = 0;
        scene::Entity entity = scene::kInvalidEntity;
        scene::Entity capEntity = scene::kInvalidEntity;   // the interior-material cap entity, a child of `entity`
        i32 body = 0;
        bool alive = true;
        bool debris = false;
        bool empty = false;
        bool skinned = false;
        bool anchored = false;
        bool released = false;
        bool frozen = false;
        bool needsRebuild = false;
        bool needsRead = true;
        bool plastic = false;              // some beam has a set: a rebuild re-takes the bend rest angles
        bool needsBody = false;            // planTear made or changed it: commitTear builds the body and the draw arrays
        bool pendingKill = false;          // planTear found nothing left: commitTear removes it
        bool baked = false;                // settled: no body, drawn as a plain static mesh
        bool fieldHeld = false;
        bool capsHidden = false;
        bool needsPack = true;
        bool capsOn = false;
        u32 heldTick = 0;
        u32 srcPiece = kNone;              // planTear: the piece whose body this one's starting state comes from
        std::vector<i32> stateMap;         // planTear: new vertex -> source body vertex
        f32 massKg = 0.0f;
        u32 epoch = 0;
        u32 lodTier = 9;
        f32 age = 0.0f, asleepFor = 0.0f, shrinkT = 0.0f, hotFor = 0.0f;
        bool shrinking = false;
        Vec3 origin{0, 0, 0};              // debris draw origin (fixed at creation, moved to the centroid when baked)
        Vec3 centroid{0, 0, 0};
        Vec3 boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
        std::vector<u32> particles;
        std::vector<u32> beams, triangles;
        std::vector<u32> renderVerts;
        // Draw arrays are [front | back skin | walls]. With an interior material the walls go to the cap entity and the main arrays are [front | back skin].
        u32 frontCount = 0, frontIndexCount = 0;
        std::vector<u32> drawIndices;
        std::vector<u32> sourceVertex;
        std::vector<f32> drawUv;
        std::vector<u32> drawWeld;         // front draw vertex -> surface point identity (for caps)
        std::vector<u32> drawEvalTri;      // proxy: front draw vertex -> proxy triangle it is evaluated against
        std::vector<Vec3> drawWorld;       // per draw vertex, refreshed when awake
        CapGeometry caps;
        std::vector<Vec3> normals;
        std::vector<Vec3> capWorld;
        std::vector<u32> capIndices, capSource;
        std::vector<f32> capUv;
        std::vector<f32> posBuf, velBuf;
        std::vector<i32> edgeSlot, bendSlot;
    };

    struct Spec {
        u64 meshId = 0, assetId = 0;
        u32 preset = 0, flags = 0;
        f32 scaleA = 1.0f, scaleB = 1.0f, scaleC = 1.0f;
        bool operator==(const Spec& o) const {
            return meshId == o.meshId && assetId == o.assetId && preset == o.preset && (flags & ~kClothRelease) == (o.flags & ~kClothRelease) &&
                   scaleA == o.scaleA && scaleB == o.scaleB && scaleC == o.scaleC;
        }
    };

    struct Entry {
        scene::Entity entity = scene::kInvalidEntity;
        bool destructible = false;
        bool legacy = false;
        bool failed = false;
        Spec spec;
        SoftAsset asset;
        const fmt::OcMeshData* mesh = nullptr;
        SimMesh sim;
        Mat4 invBuildWorld;                // W0^-1: brings the palette into the build frame
        Mat4 lastWorld;
        bool hasLastWorld = false;
        bool hardSkinNext = true;
        u32 jointCount = 1;
        bool propPalette = true;           // no skeleton: one identity joint
        scene::Entity skeletonEntity = scene::kInvalidEntity;
        std::vector<Piece> pieces;
        std::vector<u32> owner;
        std::vector<u32> renderSrc;
        u32 nextPieceId = 1;
        u32 brokenBeams = 0;
        f32 windScale = 1.0f, freezeDistance = 0.0f;
        f32 maxStrain = 0.0f;
        u32 stepAccum = 0;
        bool released = false;
        bool assetStale = false;
        bool hidden = false;               // the root is hidden because every triangle is gone
        bool conflict = false;             // both CCloth and CDestructible
        u32 totalBeams = 0;
        bool proxyOn = false;
        f32 uniformScale = 1.0f;           // the scale inside the build matrix; cap thickness and proxy offsets scale by it
        bool openSheet = false;            // the intact visible mesh has boundary edges (BackSkin::Auto)
        bool overCap = false;              // failed: the mesh welds to more particles than allowed
        bool capsLogged = false;
        i32 interiorToken = 0;             // aver_scene_material of caps.interiorMaterial; 0 = merge the walls
        bool forceAsleep = false;
        std::vector<std::vector<u32>> tearLog;   // beams that broke between two tear-pipeline runs, in order
        std::vector<u8> logged;
        std::vector<std::pair<Vec3, u32>> pendingDust;   // dust clusters planTear found, flushed by commitTear
        std::vector<u8> bakedBeams;        // beam -> belongs to a baked piece (empty until one bakes)
        HitInfo hit;
        f32 fxCooldown[kFxSlotCount] = {0, 0, 0, 0, 0};
        bool proxyMode = false;
        ProxyBind proxyBind;
        std::vector<u32> renderWeld;       // authored vertex -> welded position id (proxy mode)
        std::vector<Vec3> proxyNormals;
        u32 proxyNormalsTick = 0xFFFFFFFFu;
        u64 saveMeshKey = 0;
        std::unique_ptr<SoftState> restore;   // the blob being replayed by initialPiece
        std::vector<f32> restorePos, restoreVel;
        f32 pressureKPa = 0.0f;            // live inflation pressure (a closed cloth); a rebuild passes it on
        f32 leakKPaPerS = 0.0f;
        bool groupApplied = false;
        i32 groupHandle = 0;               // the ragdoll collision group a rebuild passes on
    };

    struct InboxItem {
        enum class Kind : u8 { Damage, Explode, Tear, Repair, Release, Wind, Budget, Preset, Pulse, FieldStart, FieldStop, Asleep, ClearDebris } kind = Kind::Damage;
        scene::Entity entity = scene::kInvalidEntity;
        Vec3 point{0, 0, 0}, dir{0, 0, 0};
        f32 radius = 0.0f, energy = 0.0f, impulse = 0.0f, a = 0.0f, b = 0.0f, c = 0.0f;
        u8 depositKind = 2;
        u32 preset = 0;
        Budget budget;
        FieldSpec field;
        u32 handle = 0;
        f32 lifetime = 0.0f;
        bool flag = false;
    };

    struct Transient {
        FieldSpec spec;
        f32 remainingS = 0.0f;
    };

    // The build inputs an entry is keyed on, read from its components (meshId left to the caller). Equal specs mean no rebuild.
    static Spec specFromComponents(const scene::CSoftBody& sb, const CCloth* cl, const CDestructible* de);
    void syncEntries(scene::World& world);
    bool buildEntry(scene::World& world, Entry& en);
    void destroyEntry(scene::World& world, Entry& en);
    void destroyPiece(scene::World& world, Entry& en, Piece& p);
    void publish(scene::World& world);
    void emit(Entry& en, const Piece* p, SoftEventKind kind, u32 beam, const Vec3& point, f32 value);
    void refreshOut(scene::World& world, Entry& en);
    void refreshDraw(Entry& en, Piece& p);
    void frontPositions(Entry& en, Piece& p);
    Entry* find(scene::Entity e);
    const Entry* find(scene::Entity e) const;
    const SoftAsset* assetFor(u64 id);
    void readback(Entry& en);
    void installLayers();
    u32 fieldTargetOf(const Entry& en, const Piece& p) const;

    // Makes (or replaces, when replaceBody != 0) the Jolt body for piece p. statePos/stateVel (3 floats per vertex: cm, cm/s) seed a restored body.
    bool buildPieceBody(Entry& en, Piece& p, i32 replaceBody, i32 stateSrcBody, const std::vector<i32>* stateMap, bool carryPose,
                        const f32* statePos = nullptr, const f32* stateVel = nullptr);
    bool initialPiece(scene::World& world, Entry& en);
    void fillPieceDraw(Entry& en, Piece& p);

    void applyInbox(scene::World& world);
    void pollImpacts(scene::World& world);
    void depositOn(scene::World& world, Entry& en, Piece* hint, const DamageDeposit& d, f32 impactScale, bool kick, SoftEventKind evKind);
    void plasticStep(scene::World& world, Entry& en, f32 dt);
    void debrisLifecycle(scene::World& world, f32 dt);
    void evictDebris(scene::World& world);
    void noteBroken(scene::World& world, Entry& en, const std::vector<u32>& beams, const Vec3& point);
    Piece* pieceById(Entry& en, u32 id);
    scene::Entity makeDebrisEntity(scene::World& world, Entry& en, Piece& p);
    void releaseEntry(scene::World& world, Entry& en);
    void repairEntry(scene::World& world, Entry& en);

    // planTear: topology and piece table (no physics, entities or events). commitTear: bodies, debris entities and events.
    // Live ticks call both back to back.
    bool tearPipeline(scene::World& world, Entry& en);
    bool planTear(scene::World& world, Entry& en);
    void commitTear(scene::World& world, Entry& en);
    void appendTearLog(Entry& en);
    void killPiece(scene::World& world, Entry& en, Piece& p);

    void fieldStep(scene::World& world, f32 stepSeconds);
    u32 applyField(scene::World& world, const FieldSpec& f, f32 seconds, bool once);
    void releaseAnchorsIn(scene::World& world, Entry& en, const std::vector<u32>& particles);
    u32 piecesInReach(const FieldSpec& f) const;
    // Alive, not baked or pending kill, targeted by the field, and inside its bounds.
    bool inReach(const Entry& en, const Piece& p, const FieldSpec& f) const;
    void holdAsleep(Piece& p);

    void buildPieceCaps(scene::World* world, Entry& en, Piece& p);
    void refreshCaps(Entry& en, Piece& p);
    void ensureCapEntity(scene::World& world, Entry& en, Piece& p);
    void capLod(scene::World& world);
    void bakePiece(scene::World& world, Entry& en, Piece& p);
    void publishVisibility(scene::World& world, Entry& en, Piece& p, bool& culled);

    void emitFx(Entry& en, FxSlot slot, const Vec3& pos, const Vec3& dir, f32 value, u32 particles = 0);
    void flushFx();

    // Builds a root piece from the entry's pending restore blob: replays the saved tear log, then makes the bodies.
    void restoreBuild(scene::World& world, Entry& en);

    bool buildProxy(scene::World& world, Entry& en, const SimMeshInput& authored, SimBuildSpec& spec, bool& notReady);
    void proxyPositions(Entry& en, Piece& p);

    void setEntryPressure(Entry& en, f32 kPa);              // en.pressureKPa and every piece's body
    void applyPressureOps();                                // the queue setPressure fills, in arrival order
    void applyGroup(Entry& en);                             // "ragdollOf": "owner", once the ragdoll exists

    void clothStep(scene::World& world, Entry& en, f32 dt);
    void applyLodAndFreeze(scene::World& world, f32 dt);
    void setCharacterProxy(scene::World& world, Entry& en, bool on);

    scene::World* world_ = nullptr;
    Options opt_;
    Stats stats_;
    Vec3 viewPos_{0, 0, 0};
    Vec3 wind_{0, 0, 0};
    std::map<scene::Entity, Entry> entries_;
    std::vector<InboxItem> inbox_;
    std::vector<SoftEvent> events_;
    std::vector<PieceView> views_;
    std::unordered_map<i32, std::pair<scene::Entity, u32>> byBody_;
    std::map<scene::Entity, u32> proxyUsers_;
    u32 nextSeq_ = 1;
    u32 stepClock_ = 0;
    u32 tickCount_ = 0;
    u32 fieldTick_ = 0;
    f32 stepSeconds_ = 1.0f / 60.0f;   // this tick's fixed-step time, what a perSecond field integrates over
    u32 rebuildsThisTick_ = 0, buildsThisTick_ = 0, depositsThisTick_ = 0;
    u32 liveDebris_ = 0;
    bool layersInstalled_ = false;
    bool installed_ = false;
    bool needFlush_ = false;           // debris entities were created: composed world matrices must be current
    bool restoring_ = false;           // replaying a saved tear log: no events, no effects

    std::map<u32, Transient> fields_;
    std::set<u32> liveHandles_;                        // issued and not stopped (also those still in the inbox)
    u32 nextFieldHandle_ = 1;
    std::map<scene::Entity, f32> spawnedLife_;
    bool effectsOn_ = true;
    SoundFn soundFn_ = nullptr;
    void* soundUser_ = nullptr;
    std::vector<FxRequest> fxQueue_;
    u32 fxSeq_ = 1;
    u32 fxDroppedThisTick_ = 0;
    // saved damage waiting for its entity's entry
    std::unordered_map<scene::Entity, std::unique_ptr<SoftState>> pendingRestore_;
    std::unordered_map<scene::Entity, u32> proxyWait_;
    struct ViewSource { ViewSourceFn fn = nullptr; void* user = nullptr; };
    std::vector<ViewSource> viewSources_;
    usize baseViewCount_ = 0;                          // views_ before the view sources appended theirs
    std::vector<std::pair<scene::Entity, f32>> pressureOps_;
    std::unordered_map<u64, fmt::OcMeshData> synthMeshes_;
    RagdollGroupFn ragdollGroupFn_ = nullptr;
    void* ragdollGroupUser_ = nullptr;

    MeshSourceFn meshFn_ = nullptr;
    void* meshUser_ = nullptr;
    AssetSourceFn assetFn_ = nullptr;
    void* assetUser_ = nullptr;
    PaletteFn paletteFn_ = nullptr;
    void* paletteUser_ = nullptr;
    bool paletteSet_ = false;
    ProxyFn proxyFn_ = nullptr;
    void* proxyUser_ = nullptr;
    NotifyFn notifyFn_ = nullptr;
    void* notifyUser_ = nullptr;
    BoneIndexFn boneFn_ = nullptr;
    void* boneUser_ = nullptr;
    PathFn pathFn_ = nullptr;
    void* pathUser_ = nullptr;
    std::unordered_map<u64, std::unique_ptr<fmt::OcMeshData>> meshCache_;
    std::unordered_map<u64, std::unique_ptr<SoftAsset>> assetCache_;
};

// Process singleton, like anim::animSystem().
SoftBodySystem& softBodySystem();

// Framework relay provider (framework_softbody_abi.h); declared here so the system need not link the framework.
// The composition root installs it: aver_fw_set_softbody_provider(&softbody::softBodyProvider, &softbody::softBodySystem()).
i32 softBodyProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

// Soft-field relay provider (framework_softfield_abi.h), installed the same way: aver_fw_set_softfield_provider(&softbody::softFieldProvider, &softbody::softBodySystem()).
i32 softFieldProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

} // namespace aver::softbody
