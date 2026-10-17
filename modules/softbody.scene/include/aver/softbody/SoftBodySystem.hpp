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
// through attachCloth / attachDestructible, never world.addComponent directly: storage arrives zero-filled and
// runs no constructor, and zero-filled storage means the useful state (scales of 0 read as 1).
//
// DETERMINISM. Inputs are the asset bytes + preset, the ordered inbox (damage, explode, tear, repair, release,
// wind, budget), the entity transforms and palettes per tick and the fixed step count. Every loop runs in
// ascending entity id, then piece id; piece and event ids come from counters advanced in that order. State is a
// pure function of those on one binary; cross-platform determinism is not claimed.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/scene/World.hpp"
#include "aver/softbody/Damage.hpp"
#include "aver/softbody/Islands.hpp"
#include "aver/softbody/SimMesh.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::softbody {

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

// ---- options, events, views -----------------------------------------------------------------------

struct Options {
    u32 maxLivePieces = 48;             // debris pieces beyond the roots
    u32 maxActiveParticles = 20000;
    u32 maxRebuildsPerFrame = 2;        // tear pipelines, resident builds and draw-mesh epochs together
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
    scene::Entity entity = scene::kInvalidEntity;   // the entity that draws it (the root, or a debris entity)
    u32 piece = 0;
    u32 epoch = 0;                    // changes when the draw mesh must be rebuilt (a tear, a release)
    u32 vertexCount = 0;
    const Vec3* world = nullptr;      // world cm per draw vertex
    const u32* indices = nullptr;     // visible triangles, into the draw vertices
    u32 indexCount = 0;
    const u32* sourceVertex = nullptr;   // draw vertex -> vertex of the entity's authored mesh (UV and rest data)
    u64 meshId = 0;                   // the entity's authored mesh
    Vec3 origin{0, 0, 0};             // debris: pack relative to this (the entity has an identity-rotation CLocal at it)
    Vec3 centroid{0, 0, 0};           // shrink target, world cm
    bool useEntityWorld = true;       // root piece: pack through the entity's CWorld inverse
    f32 scale01 = 1.0f;               // despawn shrink toward the centroid
    bool frozen = false;              // last packed vertices are still current
    bool empty = false;               // nothing to draw (every triangle gone)
};

struct Stats {
    u32 entries = 0, pieces = 0, debris = 0;
    u32 activeParticles = 0, awakePieces = 0, frozenPieces = 0;
    u32 rebuildsLastTick = 0, depositsLastTick = 0, eventsPending = 0;
};

// Per-entity query for graphs and scripts.
struct SoftBodyState {
    bool found = false;
    f32 damage01 = 0.0f, maxStrain = 0.0f;
    u32 pieces = 0, brokenEdges = 0;
    bool awake = false, frozen = false;
};

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

class SoftBodySystem {
public:
    SoftBodySystem();
    ~SoftBodySystem();
    SoftBodySystem(const SoftBodySystem&) = delete;
    SoftBodySystem& operator=(const SoftBodySystem&) = delete;

    // Registers CCloth and CDestructible (idempotent) and returns CCloth's type id.
    static u32 registerComponents(scene::World& world);
    // Attach with defaults over the zero-filled bytes, and add CSoftBody if absent. Null when the entity has a
    // mesh-less renderer, is invalid, or already carries the other kind (one body per entity root).
    static CCloth* attachCloth(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset = 0);
    static CDestructible* attachDestructible(scene::World& world, scene::Entity e, SoftPreset preset, u64 asset = 0);
    static u32 clothType();
    static u32 destructibleType();

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
    void setViewPos(const Vec3& p) { viewPos_ = p; }

    // Once per frame after the physics step and animSystem().tick, before the draw scene updates.
    void tick(scene::World& world, f32 dt);
    // Destroys bodies, debris entities and every entry; keeps options and wind.
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
    Options& options() { return opt_; }
    const Options& options() const { return opt_; }

    // ---- outputs ----
    const PieceView* pieces(u32& count) const;
    u32 drainEvents(SoftEvent* out, u32 max);
    Stats stats() const { return stats_; }
    SoftBodyState state(scene::Entity e) const;
    // The decoded mesh behind an id (the draw side reads UVs from it), or null.
    const fmt::OcMeshData* meshData(u64 meshId);
    u32 pieceCount(scene::Entity e) const;
    // For the strain debug overlay: the cage of an entity, or null. Valid until the next tick.
    const Cage* cageOf(scene::Entity e) const;
    // The world wind as last applied (cm/s), for the debug overlay.
    Vec3 windVelocity() const { return wind_; }

private:
    // One body of an entry: the root, or a detached island. Owns a subset of the entry's cage.
    struct Piece {
        u32 id = 0;
        scene::Entity entity = scene::kInvalidEntity;   // draws this piece
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
        u32 epoch = 0;
        u32 lodTier = 9;
        f32 age = 0.0f, asleepFor = 0.0f, shrinkT = 0.0f, hotFor = 0.0f;
        bool shrinking = false;
        Vec3 origin{0, 0, 0};              // debris draw origin (fixed at creation)
        Vec3 centroid{0, 0, 0};
        Vec3 boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
        std::vector<u32> particles;        // cage particle ids, in Jolt vertex order
        std::vector<u32> beams, triangles; // cage ids
        std::vector<u32> renderVerts;      // entry binding render vertex per draw vertex
        std::vector<u32> drawIndices;      // visible triangles into draw vertices
        std::vector<u32> sourceVertex;     // draw vertex -> authored mesh vertex
        std::vector<Vec3> drawWorld;       // per draw vertex, refreshed when awake
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
    };

    struct InboxItem {
        enum class Kind : u8 { Damage, Explode, Tear, Repair, Release, Wind, Budget, Preset } kind = Kind::Damage;
        scene::Entity entity = scene::kInvalidEntity;
        Vec3 point{0, 0, 0}, dir{0, 0, 0};
        f32 radius = 0.0f, energy = 0.0f, impulse = 0.0f, a = 0.0f, b = 0.0f, c = 0.0f;
        u8 depositKind = 2;
        u32 preset = 0;
        Budget budget;
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
    Entry* find(scene::Entity e);
    const Entry* find(scene::Entity e) const;
    const SoftAsset* assetFor(u64 id);
    void readback(Entry& en);
    void installLayers();

    // ---- SoftBodyBuild.cpp ----
    // Makes (or replaces, when replaceBody != 0) the Jolt body for the particles/beams/triangles of `p`.
    bool buildPieceBody(Entry& en, Piece& p, i32 replaceBody, i32 stateSrcBody, const std::vector<i32>* stateMap, bool carryPose);
    bool initialPiece(Entry& en);
    void fillPieceDraw(Entry& en, Piece& p);

    // ---- SoftBodyDamage.cpp ----
    void applyInbox(scene::World& world);
    void pollImpacts(scene::World& world);
    void depositOn(scene::World& world, Entry& en, Piece* hint, const DamageDeposit& d, f32 impactScale, bool kick, SoftEventKind evKind);
    void plasticStep(scene::World& world, Entry& en, f32 dt);
    bool tearPipeline(scene::World& world, Entry& en);
    void debrisLifecycle(scene::World& world, f32 dt);
    void evictDebris(scene::World& world);
    void noteBroken(scene::World& world, Entry& en, const std::vector<u32>& beams, const Vec3& point);
    Piece* pieceById(Entry& en, u32 id);
    scene::Entity makeDebrisEntity(scene::World& world, Entry& en, Piece& p);
    void releaseEntry(scene::World& world, Entry& en);
    void repairEntry(scene::World& world, Entry& en);

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
    u32 rebuildsThisTick_ = 0, buildsThisTick_ = 0, depositsThisTick_ = 0;
    u32 liveDebris_ = 0;
    bool layersInstalled_ = false;
    bool installed_ = false;
    bool needFlush_ = false;           // debris entities were created: composed world matrices must be current

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

} // namespace aver::softbody
