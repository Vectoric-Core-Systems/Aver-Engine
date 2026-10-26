// SoftBodySystem: cloth and destruction on scene entities, over Jolt soft bodies.
//
// The one runtime owner of every Jolt soft body that belongs to an entity (docs/SOFTBODY.md). It builds a
// cage from the entity's mesh (Aver.SoftBody: weld, beams, bends, masses, pins), makes the Jolt body through
// aver_phys_sb_*, mirrors the simulated positions back, runs the damage step (dents are in-place writes to
// the body's edge rest lengths; tears rebuild the body under the same handle and split islands into debris
// pieces), and publishes PieceViews that Aver.Render.SoftBody draws.
//
// COMPONENTS ARE REGISTERED DYNAMICALLY, the CMotionMatcher pattern: Components.hpp ids and the scene ABI do
// not change. CSoftBody (id 13) stays the marker the draw seam keys on and the legacy jiggle path. Attach
// through attachCloth / attachDestructible / attachSoftField, never world.addComponent directly: storage arrives
// zero-filled and runs no constructor, and zero-filled storage means the useful state (scales of 0 read as 1).
//
// WAVE 10 (docs/plans/wave10/SOFTBODY_DEPTH.md): thick breaks (back skin and walls as derived draw geometry), fields
// (volumes that push, hurt, release, kill or hold soft bodies), break effects (particles and sounds bound in .ocsoft),
// saved damage (the `softbody` $State channel), proxy cages for meshes over the particle cap, and cheaper debris
// (reveal handshake, baking of settled pieces, distance cadence). The pure parts live in Aver.SoftBody.
//
// DETERMINISM. Inputs are the asset bytes + preset, the ordered inbox (damage, explode, tear, repair, release,
// wind, budget, pulses), the entity transforms and palettes per tick and the fixed step count. Every loop runs in
// ascending entity id, then piece id; piece and event ids come from counters advanced in that order. Fields run
// pulses (arrival order), then component fields (ascending entity id), then transient fields (ascending handle),
// with time from the step count. State is a pure function of those on one binary; cross-platform determinism is
// not claimed.
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

// A draw vertex that is not an authored vertex (a back-skin or wall vertex of a cap): `sourceVertex` carries this.
inline constexpr u32 kGeneratedVertex = 0xFFFFFFFFu;

// ---- components -----------------------------------------------------------------------------------

inline constexpr u32 kClothNoWorld = 0x1;            // negative sense: zero-filled = collides with the world
inline constexpr u32 kClothNoCharacterProxy = 0x2;
inline constexpr u32 kClothNoWind = 0x4;
inline constexpr u32 kClothTearable = 0x8;
inline constexpr u32 kClothRelease = 0x10;           // script sets, the system clears

inline constexpr u32 kDestructNoDebris = 0x1;
inline constexpr u32 kDestructIndestructible = 0x2;  // damage is ignored

// CCloth::outState / CDestructible::outState bits.
inline constexpr u32 kStateAwake = 0x1, kStateFrozen = 0x2, kStateReleased = 0x4, kStateAssetStale = 0x8;
inline constexpr u32 kStateOverCap = 0x10;           // the mesh welds to more particles than allowed and has no proxy: authored mesh, not simulated
inline constexpr u32 kStateProxy = 0x20;             // simulated on a cooked proxy cage
inline constexpr u32 kStatePieceShift = 8;           // bits 8+ hold the live piece count

struct CCloth {
    u64 asset = 0;                // .ocsoft ObjectId; 0 = preset only
    u32 preset = 0;               // SoftPreset
    u32 flags = 0;
    f32 windScale = 0;            // 0 reads 1
    f32 stiffnessScale = 0;       // 0 reads 1 (scales compliance by 1/s)
    f32 reachScale = 0;           // 0 reads 1 (scales skinned reach)
    f32 freezeDistanceCm = 0;     // 0 = system default
    f32 outMaxStrain = 0;         // OUT
    u32 outState = 0;             // OUT
};

struct CDestructible {
    u64 asset = 0;
    u32 preset = 0;
    u32 flags = 0;
    f32 toughnessScale = 0;       // 0 reads 1 (multiplies toughnessJ and the strain limits)
    f32 impactScale = 0;          // 0 reads 1 (multiplies deposited energy)
    f32 debrisLifetimeS = 0;      // 0 = asset default
    u32 maxPieces = 0;            // 0 = asset default
    f32 outDamage = 0;            // OUT: broken beams / total, 0..1
    u32 outBroken = 0, outPieces = 0, outState = 0;
};
static_assert(sizeof(CCloth) == 40 && sizeof(CDestructible) == 48, "the soft-body components must be padding-free");

inline constexpr u32 kFieldDisabled = 0x1;   // negative sense: zero-filled = enabled
inline constexpr u32 kFieldOnce = 0x2;       // apply once as a pulse, then set kFieldDisabled

// CSoftField::outState bits.
inline constexpr u32 kFieldStateActive = 0x1, kFieldStateFired = 0x2;

// A placed soft-body field: the entity's world matrix is the shape frame. Continuous (strength in cm/s per second,
// energy in joules per second) unless kFieldOnce, which applies strength and energy once as a pulse.
struct CSoftField {                  // "CSoftField"
    u32 kind = 0, shape = 0, flags = 0, targets = 0;      // targets 0 reads kFieldTargetAll, except Kill which reads Debris only
    f32 extentX = 0, extentY = 0, extentZ = 0;            // 0 reads 100
    f32 strength = 0, energyJPerS = 0, falloff = 0;       // falloff 0 reads 1
    f32 dirX = 0, dirY = 0, dirZ = 0;                     // all zero reads +Z for DirectionalForce
    f32 turbulence = 0;
    u32 outTouched = 0;               // OUT: pieces in reach on the last tick
    u32 outState = 0;                 // OUT: bit0 active, bit1 fired (a kFieldOnce field after it ran)
};
static_assert(sizeof(CSoftField) == 64, "CSoftField must be padding-free");

// ---- options, events, views -----------------------------------------------------------------------

struct Options {
    u32 maxLivePieces = 48;             // debris pieces beyond the roots
    u32 maxActiveParticles = 20000;
    u32 maxRebuildsPerFrame = 2;        // tear pipelines and entry builds
    u32 maxDrawBuildsPerFrame = 8;      // resident builds and draw-mesh epochs in the draw side (Aver.Render.SoftBody)
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
    f32 hotSeconds = 2.0f;              // a piece is "hot" this long after an impact or a deposit
    f32 watchStrain = 0.005f;           // ... or while its largest strain exceeds this
    // ---- wave 10 ----
    bool capsEnabled = true;            // master switch for thick breaks (an asset's caps.mode still applies)
    bool bakeEnabled = true;            // settled debris becomes a plain static mesh
    f32 bakeAfterS = 3.0f;              // asleep this long before it is baked
    f32 cullPixelRatio = 0.004f;        // debris and cap entities whose bounding radius over distance is below this are not drawn
    u32 fxMaxPerTick = 16;              // break effects dispatched per tick
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
    scene::Entity entity = scene::kInvalidEntity;   // the entity that draws it (the root, or a debris entity, or a cap entity)
    u32 piece = 0;
    u32 epoch = 0;                    // changes when the draw mesh must be rebuilt (a tear, a release, a cap change, a bake)
    u32 vertexCount = 0;
    const Vec3* world = nullptr;      // world cm per draw vertex
    const u32* indices = nullptr;     // visible triangles, into the draw vertices
    u32 indexCount = 0;
    const u32* sourceVertex = nullptr;   // draw vertex -> vertex of the entity's authored mesh (UV and rest data), or kGeneratedVertex
    u64 meshId = 0;                   // the entity's authored mesh
    Vec3 origin{0, 0, 0};             // debris: pack relative to this (the entity has an identity-rotation CLocal at it)
    Vec3 centroid{0, 0, 0};           // shrink target, world cm
    bool useEntityWorld = true;       // root piece: pack through the entity's CWorld inverse
    f32 scale01 = 1.0f;               // despawn shrink toward the centroid
    bool frozen = false;              // last packed vertices are still current
    bool empty = false;               // nothing to draw (every triangle gone)
    // ---- wave 10 ----
    const f32* uv = nullptr;          // 2 per draw vertex; null means every vertex is authored. Read for kGeneratedVertex entries
    u32 layer = 0;                    // 0 surface (with merged caps), 1 the interior-material cap entity
    bool revealOnReady = false;       // debris and cap entities start invisible: set the visible bit once this view has a resident
    bool bake = false;                // a settled piece: build one plain static mesh from these vertices and never pack again
};

struct Stats {
    u32 entries = 0, pieces = 0, debris = 0;
    u32 activeParticles = 0, awakePieces = 0, frozenPieces = 0;
    u32 rebuildsLastTick = 0, depositsLastTick = 0, eventsPending = 0;
    // ---- wave 10 ----
    u32 bakedPieces = 0, heldPieces = 0, fieldsApplied = 0, transientFields = 0;
    u32 fxSent = 0, fxDropped = 0;
};

// Per-entity query for graphs and scripts.
struct SoftBodyState {
    bool found = false;
    f32 damage01 = 0.0f, maxStrain = 0.0f;
    u32 pieces = 0, brokenEdges = 0;
    bool awake = false, frozen = false;
};

// The last damage deposit on an entity, for the Tuning HUD (docs/SOFTBODY.md "Tuning").
struct HitInfo {
    bool valid = false;
    u8 kind = 0;                      // DepositKind
    f32 closingCmS = 0.0f;            // an impact's closing speed, else 0
    f32 energyJ = 0.0f, radiusCm = 0.0f;
    u32 beamsDamaged = 0, broken = 0;
    f32 maxStrain = 0.0f;             // now, not at the hit
    f32 maxSetCm = 0.0f;              // the largest permanent set now, cm
    u32 tick = 0;
    u32 pieces = 0, baked = 0;
};

// Permanent set and live debris of one entity.
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
// Plays one sound request. Called at the end of the tick for requests that survive the budgets.
using SoundFn = void (*)(const FxRequest& r, void* user);

// ---- host seams -----------------------------------------------------------------------------------

// u64 meshId -> decoded mesh (host content cache); null when it does not resolve yet.
using MeshSourceFn = const fmt::OcMeshData* (*)(u64 meshId, void* user);
// u64 asset id -> parsed .ocsoft; null when absent.
using AssetSourceFn = const SoftAsset* (*)(u64 assetId, void* user);
// The skinning palette of an entity (model x inverse-bind per joint), or null.
using PaletteFn = const Mat4* (*)(scene::Entity e, u32& count, void* user);
// Starts (on = true) or stops a Kinematic ragdoll as the entity's collision proxy.
using ProxyFn = void (*)(scene::Entity e, bool on, void* user);
// The graph notify seam: (entity, "SoftBodyTorn", user).
using NotifyFn = void (*)(scene::Entity e, const char* name, void* user);
// A bone name's index in the skeleton an entity is skinned to, or -1 (for bone-weight paint rules).
using BoneIndexFn = i32 (*)(scene::Entity e, const std::string& bone, void* user);
// Turns an asset id into a path (the same function the hosts hand AnimSystem).
using PathFn = std::string (*)(u64 objectId, void* user);
// Wave 11. The physics handle of the ragdoll that belongs to a skeleton entity (the group id a soft body can ignore), or 0.
using RagdollGroupFn = i32 (*)(scene::Entity skeleton, void* user);
// Wave 11. Appends the extra PieceViews of another system (SoftShapeSystem: jelly and rope); see addViewSource.
using ViewSourceFn = void (*)(std::vector<PieceView>& out, void* user);

class SoftBodySystem {
public:
    SoftBodySystem();
    ~SoftBodySystem();
    SoftBodySystem(const SoftBodySystem&) = delete;
    SoftBodySystem& operator=(const SoftBodySystem&) = delete;

    // Registers CCloth, CDestructible and CSoftField (idempotent) and returns CCloth's type id.
    static u32 registerComponents(scene::World& world);
    // Attach with defaults over the zero-filled bytes, and add CSoftBody if absent. Null when the entity has a
    // mesh-less renderer, is invalid, or already carries the other kind (one body per entity root).
    static CCloth* attachCloth(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset = 0);
    static CDestructible* attachDestructible(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset = 0);
    // A field needs nothing else on the entity: its transform is the frame. Defaults: radial sphere of 100 cm, all targets.
    static CSoftField* attachSoftField(scene::World& world, scene::Entity e);
    static u32 clothType();
    static u32 destructibleType();
    static u32 softFieldType();

    void install(scene::World& world);
    void uninstall();

    void setMeshSource(MeshSourceFn fn, void* user) { meshFn_ = fn; meshUser_ = user; }
    void setAssetSource(AssetSourceFn fn, void* user) { assetFn_ = fn; assetUser_ = user; }
    // Installs the default mesh and asset sources: decode caches over this path resolver.
    void setPathResolver(PathFn fn, void* user);
    void setPaletteSource(PaletteFn fn, void* user) { paletteFn_ = fn; paletteUser_ = user; paletteSet_ = true; }
    void setCharacterProxyFn(ProxyFn fn, void* user) { proxyFn_ = fn; proxyUser_ = user; }
    void setNotifySink(NotifyFn fn, void* user) { notifyFn_ = fn; notifyUser_ = user; }
    void setBoneIndexFn(BoneIndexFn fn, void* user) { boneFn_ = fn; boneUser_ = user; }
    // Wave 11: which ragdoll a cloth with "group": { "ragdollOf": "owner" } ignores. Null: the group is never applied.
    void setRagdollGroupFn(RagdollGroupFn fn, void* user) { ragdollGroupFn_ = fn; ragdollGroupUser_ = user; }
    void setViewPos(const Vec3& p) { viewPos_ = p; }
    Vec3 viewPos() const { return viewPos_; }
    // Where sound requests go (the host adapter in Runtime/include/aver/game/SoftBodyEffects.hpp). Null: silent.
    void setSoundSink(SoundFn fn, void* user) { soundFn_ = fn; soundUser_ = user; }
    // The master switch for break effects (particles and sounds). Immediate.
    void setEffectsEnabled(bool on) { effectsOn_ = on; }
    bool effectsEnabled() const { return effectsOn_; }

    // Once per frame after the physics step and animSystem().tick, before the draw scene updates.
    void tick(scene::World& world, f32 dt);
    // Destroys bodies, debris entities and every entry; keeps options and wind. Also drops transient fields, queued
    // pulses, the effect queue and any pending restore blobs.
    void clear();

    // ---- inbox: applied in arrival order at the next tick ----
    bool damage(scene::Entity e, Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind);
    u32 explode(Vec3 centre, f32 radiusCm, f32 impulseCmS, f32 energyJ);   // returns entities in reach right now
    // Damage to every entity with a piece in reach (the framework relay with entity 0). Returns how many.
    u32 damageNear(Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind);
    // How many unbroken beams a cut of this radius would break right now.
    u32 cutPreview(scene::Entity e, Vec3 point, f32 radiusCm) const;
    u32 tearAt(scene::Entity e, Vec3 point, f32 radiusCm);
    bool repair(scene::Entity e);
    bool release(scene::Entity e);
    bool setPreset(scene::Entity e, u32 preset);
    void setWind(Vec3 v, f32 gust, f32 gustHz, f32 turbulence);
    void setBudget(const Budget& b);
    // Holds (true) or releases (false) every piece of the entity asleep, the way a Sleep field does.
    bool setAsleep(scene::Entity e, bool asleep);
    // Destroys the entity's debris pieces now (the shrink is skipped).
    bool clearDebris(scene::Entity e);
    // Repairs every entry; returns how many.
    u32 repairAll();
    Options& options() { return opt_; }
    const Options& options() const { return opt_; }

    // ---- fields (docs/SOFTBODY.md 2.4; VEHICLES calls pulseField/impact) ----
    // One application at the next tick, in arrival order. Returns the pieces in reach now (an upper bound of what is touched).
    u32 pulseField(const FieldSpec& f);
    // A transient continuous field (perSecond forced on) for lifetimeS seconds. Returns a handle, 0 = refused.
    u32 startField(const FieldSpec& f, f32 lifetimeS);
    bool stopField(u32 handle);
    // Damage pulse in a sphere plus a directional kick along dir (zero = radial); the crash helper. Returns the pieces in reach now.
    u32 impact(Vec3 point, Vec3 dir, f32 radiusCm, f32 energyJ, f32 kickCmS);
    // An entity carrying a CSoftField that destroys itself after lifetimeS (move it to move the field).
    scene::Entity spawnFieldEntity(scene::World& world, const FieldSpec& f, f32 lifetimeS);
    // True for an entity spawnFieldEntity made that has not expired: a save skips it, since its lifetime is not saved.
    bool isTransientFieldEntity(scene::Entity e) const { return spawnedLife_.count(e) != 0; }

    // ---- saved damage (docs/SOFTBODY.md 2.6) ----
    // Serialises the entity's damage as the `softbody` channel payload. False (and `out` untouched) when it has none.
    bool saveState(scene::Entity e, std::string& out) const;
    // Stores a payload under the (new) entity; the entry applies it when it is built. False for a malformed payload.
    bool restoreState(scene::Entity e, std::string_view payload);

    // ---- wave 11 (docs/plans/wave11/SOFTBODY_HAIR.md): inflatables and the seams SoftShapeSystem uses ----
    // Sets the pressure (kPa at rest volume) of a closed cloth; applied at the next tick with the other ops. False for an
    // entity with no cloth body, a destructible, or a negative value. 0 deflates.
    bool setPressure(scene::Entity e, f32 kPa);
    f32 pressureOf(scene::Entity e) const;
    // The physics handle of the entity's root piece, 0 when it has no live soft body.
    i32 bodyOf(scene::Entity e) const;
    // The cloth or destructible entity a physics body belongs to, kInvalidEntity when none.
    scene::Entity entityOfBody(i32 body) const;
    // Another system's PieceViews: `fn` is called at the end of every publish() and appends to the list pieces() returns, so
    // the draw side reads one list. refreshViewSources() rebuilds that tail (call it after the other system ticked).
    void addViewSource(ViewSourceFn fn, void* user);
    void removeViewSource(ViewSourceFn fn, void* user);
    void refreshViewSources();
    // A mesh that has no file (the rope tube's placeholder cube): meshData(id) returns it, before any host source.
    void registerSyntheticMesh(u64 id, const fmt::OcMeshData& mesh);
    // The parsed .ocsoft behind an id through the same sources the cloth path uses, or null.
    const SoftAsset* softAsset(u64 id) { return assetFor(id); }
    // A graph event at an entity (the notify sink the host installed); no-op without one.
    void notifyEntity(scene::Entity e, const char* name) {
        if (notifyFn_) notifyFn_(e, name, notifyUser_);
    }

    // ---- outputs ----
    const PieceView* pieces(u32& count) const;
    u32 drainEvents(SoftEvent* out, u32 max);
    Stats stats() const { return stats_; }
    SoftBodyState state(scene::Entity e) const;
    HitInfo lastHit(scene::Entity e) const;
    DentInfo dents(scene::Entity e) const;
    // The decoded mesh behind an id (the draw side reads UVs from it), or null.
    const fmt::OcMeshData* meshData(u64 meshId);
    u32 pieceCount(scene::Entity e) const;
    // For the strain debug overlay: the cage of an entity, or null. Valid until the next tick.
    const Cage* cageOf(scene::Entity e) const;
    // The world wind as last applied (cm/s), for the debug overlay.
    Vec3 windVelocity() const { return wind_; }
    // Floating-origin shift (LARGE_WORLD 2.2.5): subtracts `deltaCm` from every world position the system caches
    // across ticks: each entry's lastWorld (else one hard re-skin through teleportCm), the cage particles, piece
    // origin / centroid / bounds / draw arrays, queued hit points and field frames. The skin palette needs nothing:
    // invBuildWorld is entity-local. Call after World::shiftRoots and the physics shift.
    void rebase(const Vec3& deltaCm);
    // For the field gizmos: the live transient fields, and a component field's spec as the step would apply it.
    struct FieldView { u32 handle = 0; FieldSpec spec; f32 remainingS = 0.0f; };
    std::vector<FieldView> transientFields() const;
    static bool specOfComponent(scene::World& world, scene::Entity e, FieldSpec& out);

private:
    // One body of an entry: the root, or a detached island. Owns a subset of the entry's cage.
    struct Piece {
        u32 id = 0;
        scene::Entity entity = scene::kInvalidEntity;   // draws this piece
        scene::Entity capEntity = scene::kInvalidEntity;   // the interior-material cap entity, a child of `entity`
        i32 body = 0;
        bool alive = true;
        bool debris = false;
        bool empty = false;                // no triangles left to draw
        bool skinned = false;
        bool anchored = false;
        bool released = false;
        bool frozen = false;
        bool needsRebuild = false;         // a beam broke since the body was built
        bool needsRead = true;             // positions must be read at least once (a fresh body)
        bool plastic = false;              // some beam has a set: a rebuild must re-take the bend rest angles
        // ---- wave 10 ----
        bool needsBody = false;            // planTear made or changed it: commitTear builds the body and the draw arrays
        bool pendingKill = false;          // planTear found nothing left of it: commitTear removes it
        bool baked = false;                // settled: no body, drawn as a plain static mesh
        bool fieldHeld = false;            // a Sleep field (or setAsleep) holds it inactive
        bool capsHidden = false;           // beyond caps.lodCm: drawn without caps
        bool needsPack = true;             // positions changed since the last published view
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
        std::vector<u32> particles;        // cage particle ids, in Jolt vertex order
        std::vector<u32> beams, triangles; // cage ids
        std::vector<u32> renderVerts;      // entry binding render vertex per front draw vertex
        // The draw arrays are laid out [front | back skin | walls]. With an interior material the walls leave for the cap
        // entity's own arrays (capWorld ...) and the main arrays are [front | back skin].
        u32 frontCount = 0, frontIndexCount = 0;
        std::vector<u32> drawIndices;      // visible triangles into draw vertices (caps appended)
        std::vector<u32> sourceVertex;     // draw vertex -> authored mesh vertex or kGeneratedVertex
        std::vector<f32> drawUv;           // 2 per draw vertex
        std::vector<u32> drawWeld;         // front draw vertex -> surface point identity (for caps)
        std::vector<u32> drawEvalTri;      // proxy: front draw vertex -> proxy triangle it is evaluated against
        std::vector<Vec3> drawWorld;       // per draw vertex, refreshed when awake
        CapGeometry caps;
        std::vector<Vec3> normals;         // smooth normals of the front draw vertices
        std::vector<Vec3> capWorld;        // the interior-material cap entity's arrays
        std::vector<u32> capIndices, capSource;
        std::vector<f32> capUv;
        std::vector<f32> posBuf, velBuf;   // readback scratch (3 per vertex)
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
        bool legacy = false;               // a plain CSoftBody
        bool failed = false;
        Spec spec;
        SoftAsset asset;
        const fmt::OcMeshData* mesh = nullptr;
        SimMesh sim;
        Mat4 invBuildWorld;                // W0^-1: the palette is brought into the build frame
        Mat4 lastWorld;
        bool hasLastWorld = false;
        bool hardSkinNext = true;
        u32 jointCount = 1;
        bool propPalette = true;           // no skeleton: one identity joint
        scene::Entity skeletonEntity = scene::kInvalidEntity;
        std::vector<Piece> pieces;
        std::vector<u32> owner;            // cage particle -> piece index
        std::vector<u32> renderSrc;        // binding render vertex -> authored mesh vertex
        u32 nextPieceId = 1;
        u32 brokenBeams = 0;
        f32 windScale = 1.0f, freezeDistance = 0.0f;
        f32 maxStrain = 0.0f;
        u32 stepAccum = 0;
        bool released = false;
        bool assetStale = false;
        bool hidden = false;               // the root entity is hidden because every triangle is gone
        bool conflict = false;             // both CCloth and CDestructible
        u32 totalBeams = 0;
        bool proxyOn = false;              // this entry holds a share of its skeleton entity's collision proxy
        // ---- wave 10 ----
        f32 uniformScale = 1.0f;           // the scale inside the build matrix; cap thickness and proxy offsets scale by it
        bool openSheet = false;            // the intact visible mesh has boundary edges (BackSkin::Auto)
        bool overCap = false;              // failed because the mesh welds to more particles than allowed
        bool capsLogged = false;           // the over-limit warning was logged
        i32 interiorToken = 0;             // aver_scene_material of caps.interiorMaterial, 0 = merge the walls
        bool forceAsleep = false;          // setAsleep(true)
        std::vector<std::vector<u32>> tearLog;   // beams that broke between two tear-pipeline runs, in order
        std::vector<u8> logged;            // beam -> already in the log
        std::vector<std::pair<Vec3, u32>> pendingDust;   // dust clusters planTear found, flushed by commitTear
        std::vector<u8> bakedBeams;        // beam -> belongs to a baked piece (empty until one bakes)
        HitInfo hit;
        f32 fxCooldown[kFxSlotCount] = {0, 0, 0, 0, 0};
        // proxy cage
        bool proxyMode = false;
        ProxyBind proxyBind;
        std::vector<u32> renderWeld;       // authored vertex -> welded position id (proxy mode)
        std::vector<Vec3> proxyNormals;
        u32 proxyNormalsTick = 0xFFFFFFFFu;
        // saved damage
        u64 saveMeshKey = 0;
        std::unique_ptr<SoftState> restore;   // the blob being replayed by initialPiece
        std::vector<f32> restorePos, restoreVel;
        // ---- wave 11 ----
        f32 pressureKPa = 0.0f;            // the live inflation pressure (a closed cloth); a rebuild passes it on
        f32 leakKPaPerS = 0.0f;
        bool groupApplied = false;         // the ragdoll collision group was set
        i32 groupHandle = 0;               // ... to this (a rebuild passes it on)
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

    // ---- SoftBodySystem.cpp ----
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

    // ---- SoftBodyBuild.cpp ----
    // Makes (or replaces, when replaceBody != 0) the Jolt body for the particles/beams/triangles of `p`. statePos/stateVel
    // (3 floats per piece vertex, world cm and cm/s) give the starting state of a restored body.
    bool buildPieceBody(Entry& en, Piece& p, i32 replaceBody, i32 stateSrcBody, const std::vector<i32>* stateMap, bool carryPose,
                        const f32* statePos = nullptr, const f32* stateVel = nullptr);
    bool initialPiece(scene::World& world, Entry& en);
    void fillPieceDraw(Entry& en, Piece& p);

    // ---- SoftBodyDamage.cpp ----
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

    // ---- SoftBodyTear.cpp ----
    // The tear pipeline split in two: planTear is topology and the piece table (no physics, no entities, no events);
    // commitTear makes the bodies, the debris entities and the events. Live ticks call them back to back.
    bool tearPipeline(scene::World& world, Entry& en);
    bool planTear(scene::World& world, Entry& en);
    void commitTear(scene::World& world, Entry& en);
    void appendTearLog(Entry& en);
    void killPiece(scene::World& world, Entry& en, Piece& p);

    // ---- SoftBodyFields.cpp ----
    void fieldStep(scene::World& world, f32 stepSeconds);
    // Applies one field to every entry; returns the pieces in reach. `once` makes it a pulse (strength and energy applied once).
    u32 applyField(scene::World& world, const FieldSpec& f, f32 seconds, bool once);
    void releaseAnchorsIn(scene::World& world, Entry& en, const std::vector<u32>& particles);
    u32 piecesInReach(const FieldSpec& f) const;

    // ---- SoftBodyCaps.cpp (thick breaks, cap entities, baking, draw cadence) ----
    void buildPieceCaps(scene::World* world, Entry& en, Piece& p);
    void refreshCaps(Entry& en, Piece& p);
    void ensureCapEntity(scene::World& world, Entry& en, Piece& p);
    void capLod(scene::World& world);
    void bakePiece(scene::World& world, Entry& en, Piece& p);
    void publishVisibility(scene::World& world, Entry& en, Piece& p, bool& culled);

    // ---- SoftBodyFx.cpp ----
    void emitFx(Entry& en, FxSlot slot, const Vec3& pos, const Vec3& dir, f32 value, u32 particles = 0);
    void flushFx();

    // ---- SoftBodySave.cpp ----
    // Builds a root piece from the entry's pending restore blob: replays the saved tear log, then makes the bodies.
    void restoreBuild(scene::World& world, Entry& en);

    // ---- SoftBodyProxy.cpp ----
    bool buildProxy(scene::World& world, Entry& en, const SimMeshInput& authored, SimBuildSpec& spec, bool& notReady);
    void proxyPositions(Entry& en, Piece& p);

    // ---- SoftBodyBuild.cpp (wave 11) ----
    void setEntryPressure(Entry& en, f32 kPa);              // en.pressureKPa and every piece's body
    void applyPressureOps();                                // the queue setPressure fills, in arrival order
    void applyGroup(Entry& en);                             // "ragdollOf": "owner", once the ragdoll exists

    // ---- SoftBodyCloth.cpp ----
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
    std::unordered_map<i32, std::pair<scene::Entity, u32>> byBody_;    // body handle -> (entry entity, piece id)
    std::map<scene::Entity, u32> proxyUsers_;                          // skeleton entity -> cloth users
    u32 nextSeq_ = 1;
    u32 stepClock_ = 0;                // fixed steps seen (damageEvalEveryNSteps)
    u32 tickCount_ = 0;                // ticks seen (draw cadence, the Tuning HUD's tick)
    u32 fieldTick_ = 0;                // advances once per field step (Sleep holds)
    f32 stepSeconds_ = 1.0f / 60.0f;   // this tick's fixed-step time, what a perSecond field integrates over
    u32 rebuildsThisTick_ = 0, buildsThisTick_ = 0, depositsThisTick_ = 0;
    u32 liveDebris_ = 0;
    bool layersInstalled_ = false;
    bool installed_ = false;
    bool needFlush_ = false;           // debris entities were created: composed world matrices must be current
    bool restoring_ = false;           // replaying a saved tear log: no events, no effects

    // fields
    std::map<u32, Transient> fields_;                  // ascending handle
    std::set<u32> liveHandles_;                        // issued and not stopped (also those still in the inbox)
    u32 nextFieldHandle_ = 1;
    std::map<scene::Entity, f32> spawnedLife_;         // spawnFieldEntity: seconds left
    // effects
    bool effectsOn_ = true;
    SoundFn soundFn_ = nullptr;
    void* soundUser_ = nullptr;
    std::vector<FxRequest> fxQueue_;
    u32 fxSeq_ = 1;
    u32 fxDroppedThisTick_ = 0;
    // saved damage waiting for its entity's entry
    std::unordered_map<scene::Entity, std::unique_ptr<SoftState>> pendingRestore_;
    std::unordered_map<scene::Entity, u32> proxyWait_;
    // wave 11
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

SoftBodySystem& softBodySystem();   // process singleton, like anim::animSystem()

// The framework relay's provider (framework_softbody_abi.h, aver_fw_set_softbody_provider), with the
// SoftBodySystem as `user`. Declared here so the system need not link the framework: the composition root
// hands it across: aver_fw_set_softbody_provider(&softbody::softBodyProvider, &softbody::softBodySystem()).
i32 softBodyProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

// The soft-field relay's provider (framework_softfield_abi.h, aver_fw_set_softfield_provider), installed the same way:
// aver_fw_set_softfield_provider(&softbody::softFieldProvider, &softbody::softBodySystem()).
i32 softFieldProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

} // namespace aver::softbody
