#pragma once
// HairSystem: strand hair on scene entities (docs/plans/wave11/SOFTBODY_HAIR.md 2.15, docs/HAIR.md).
//
// WHAT IT OWNS. A CHair on a character's root entity names an .ochair groom. The system builds the groom into Jolt's hair settings (Aver.Hair's
// buildHairSettings, which drives the aver_phys_hair_* ABI), shares those settings between every instance of the same groom, creates one physics hair
// per component, feeds it each tick (the head's transform, the skinning matrices of the scalp's joints, the world wind), decides how often it is
// simulated by distance, keeps kinematic convex-hull proxies at the head, neck and shoulders for the strands to collide with, and publishes one HairView
// per drawn instance for Aver.Render.Hair. The simulation itself is Jolt's, in Aver.Physics; this class never sees a Jolt type.
//
// THE DRAW CHILD. The component sits on the entity that owns the skeleton, but the strands are drawn by an ordinary mesh draw whose vertices the ribbon
// pass writes, so the system makes (or re-adopts) a CHILD entity named "Hair" with an identity local transform, a CMeshRenderer on the engine cube as a
// placeholder mesh and the groom's look material, and a CHairDraw marker that names its owner. The draw side substitutes the ribbon mesh for the
// placeholder (the SkinnedScene / SoftBodyScene seam). Because the child's world matrix equals its owner's, the ribbons are written in the owner's model
// space (the space the scalp mesh is in) and the draw applies the world matrix like any other mesh. A saved level carries the child; on load it is found
// again through its CHairDraw rather than made twice.
//
// REGISTRATION is dynamic, the CCloth pattern: attach through attachHair, never world.addComponent directly (storage arrives zero-filled and runs no
// constructor; zero in every scale field reads 1).
//
// ORDER OF A FRAME: animSystem().tick, aver_phys_step, World::flush, then tick() here (it records the hair updates and seals them with one
// aver_phys_hair_flush), then the render side's update and prePass (which replay the recorded steps on the GPU).
//
// THE COMPUTE HOST must be decided before the first groom is built: aver_phys_hair_set_compute_host is refused once any settings exist, and a groom
// built on Jolt's CPU backend stays there. installComputeHost() is the one call that does it; a host that has a GPU hands in the table of
// Aver.Render.Hair's RhiComputeHost, a headless one hands in null (the CPU backend, for grooms of cpuMaxSimStrands or fewer simulated strands).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"
#include "aver/hair/HairLod.hpp"
#include "aver/physics/physics_hair_abi.h"
#include "aver/scene/World.hpp"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::hair {

// ---- components ------------------------------------------------------------------------------------

inline constexpr u32 kHairNoCollision = 0x1;
inline constexpr u32 kHairNoShadow = 0x2;
inline constexpr u32 kHairRayTraced = 0x4;      // the strands are in the ray-tracing structure (default off; HairScene::inRayTracing)
inline constexpr u32 kHairNoWind = 0x8;
inline constexpr u32 kHairHidden = 0x10;

// CHair::outState bits; the LOD level the groom is drawn at is in bits 8 and up.
inline constexpr u32 kHairSimulating = 0x1;
inline constexpr u32 kHairFrozen = 0x2;         // beyond the cadence's far distance, or not seen for a while
inline constexpr u32 kHairInert = 0x4;          // the build failed or the device has no compute: the component does nothing
inline constexpr u32 kHairBuilding = 0x8;
inline constexpr u32 kHairLodShift = 8;

struct CHair {                       // "CHair"
    u64 asset = 0;                   // .ochair ObjectId
    u64 material = 0;                // RESERVED: an .ocmat override of the groom's LOOK (the look comes from the groom's LOOK record today)
    u32 flags = 0;
    f32 widthScale = 0;              // 0 reads 1
    f32 densityScale = 0;            // 0 reads 1: scales the fraction of the render strands drawn
    f32 lodBias = 0;                 // added to the LOD level, in steps
    f32 windScale = 0;               // 0 reads 1
    f32 stiffnessScale = 0;          // 0 reads 1 (divides the bend and stretch compliance of every material)
    f32 teleportCm = 0;              // 0 reads HairOptions::teleportCm: a root that moves further than this in one frame restarts from the default pose
    u32 reserved = 0;
    u32 outState = 0;                // OUT: kHair* bits
    u32 outRenderStrands = 0;        // OUT: strands drawn
    u32 outSimStrands = 0;           // OUT
    u32 outReserved = 0;
};
static_assert(sizeof(CHair) == 64, "CHair must be padding-free");

struct CHairDraw {                   // "CHairDraw": marks the draw child of a CHair
    u32 owner = 0;                   // the entity carrying the CHair
    u32 reserved = 0;
};
static_assert(sizeof(CHairDraw) == 8, "CHairDraw must be padding-free");

// ---- options, views, stats --------------------------------------------------------------------------

struct HairOptions {
    i32 hairLayer = 13;              // the layer hair collides as; the proxies are on proxyLayer and touch only this one
    i32 proxyLayer = 12;
    i32 freeLayer = 11;              // a hair with kHairNoCollision collides as this layer, which touches nothing
    f32 teleportCm = 200.0f;
    f32 windDragPerS = 3.0f;
    f32 hideBeyondCm = 6000.0f;      // the draw child is hidden past this distance from the view
    HairCadenceOptions cadence;
    u32 maxInstances = 8;
    u32 maxBuildsPerFrame = 1;
    u32 vramBudgetMB = 96;           // a groom whose draw buffers would exceed this is refused at build
    u32 cpuMaxSimStrands = 64;       // the Jolt CPU backend is for tests: larger grooms are refused on it
    f32 maxRootToScalpCm = 2.0f;
};

// The look of a groom, render only (the LOOK record of its first material).
struct HairLookView {
    f32 baseColor[3] = {0.35f, 0.22f, 0.12f};
    f32 tipColor[3] = {0.55f, 0.40f, 0.25f};
    f32 colorVariation = 0.15f;
    f32 roughness = 0.35f;
    f32 shift = 0.07f;
    f32 tt = 1.0f;
    f32 trt = 1.0f;
    f32 azimuthRough = 0.4f;
    f32 tint[3] = {0.9f, 0.6f, 0.4f};
    f32 scatter = 0.5f;
};

// Everything the draw side needs of one drawn instance. The pointers belong to the system's shared per-groom data and stay valid until the next tick().
struct HairView {
    scene::Entity entity = scene::kInvalidEntity;       // the owner
    scene::Entity drawEntity = scene::kInvalidEntity;   // the child whose mesh is substituted
    u64 key = 0;                                        // the groom: instances of one key share static GPU data
    i32 settings = 0;
    i32 hair = 0;
    u64 renderBuffer = 0;                               // host handle of the render positions (0 on the CPU backend)
    u32 renderStrands = 0;
    u32 renderVertices = 0;
    const i32* strandTable = nullptr;                   // {first render vertex, count} per render strand
    const f32* widthRootCm = nullptr;                   // per render strand
    const f32* widthTipCm = nullptr;
    u64 seed = 1;
    Mat4 world;                                         // hair space to world: the owner's world matrix
    f32 widthScale = 1.0f;
    f32 densityScale = 1.0f;
    f32 lodBias = 0.0f;
    u32 flags = 0;
    f32 lodDistances[3] = {300.0f, 700.0f, 1500.0f};
    f32 lodFractions[4] = {1.0f, 0.5f, 0.25f, 0.12f};
    f32 distanceCm = 0.0f;
    u32 lodLevel = 0;                                   // the strand-count level to draw (HairLod.hpp), hysteresis and lodBias applied
    HairLookView look;
    i32 materialToken = 0;                              // the interned surface name the draw child's material is
    std::string materialName;
    bool visible = true;                                // false: hidden by flag or distance, do not draw or expand
    bool settled = false;                               // the simulation has recorded at least one step: the render positions mean something
    u64 estimatedBytes = 0;                             // the draw buffers' size, for the budget
};

struct HairStats {
    u32 instances = 0, simulating = 0, frozen = 0, inert = 0, groomsCached = 0;
    u32 proxies = 0;
    u32 buildsLastTick = 0;
    u32 updatesLastTick = 0;
};

// ---- the system ------------------------------------------------------------------------------------

class HairSystem {
public:
    HairSystem();
    ~HairSystem();
    HairSystem(const HairSystem&) = delete;
    HairSystem& operator=(const HairSystem&) = delete;

    // Registers CHair and CHairDraw (idempotent) and returns CHair's type id.
    static u32 registerComponents(scene::World& world);
    static u32 hairType();
    static u32 drawType();
    // Attaches a CHair with defaults over the zero-filled bytes. Null when the entity is invalid or already carries one.
    static CHair* attachHair(scene::World& world, scene::Entity e, u64 asset);

    // The content-id to absolute-path resolver the host gives the animation system (an .ochair and its scalp mesh are found through it).
    using PathFn = std::string (*)(u64 objectId, void* user);
    void setPathResolver(PathFn fn, void* user) { pathFn_ = fn; pathUser_ = user; }

    // Decides the compute host (see the file comment). `table` may be null for Jolt's CPU backend. False when the physics layer refuses it (a
    // groom already exists). Call once, before the first tick, on the composition root's thread.
    bool installComputeHost(const AverPhysComputeHostV1* table);

    HairOptions& options() { return opt_; }
    const HairOptions& options() const { return opt_; }

    // The world wind the soft bodies feel, forwarded by the host where it already lands (SoftBodies.SetWind). cm/s and a gust amplitude in cm/s.
    void setWind(const Vec3& windCmS, f32 gustCmS) { wind_ = windCmS; gust_ = gustCmS; }

    // Once per frame, after the pose, the physics step and World::flush. `view` is the camera position in world centimetres.
    void tick(scene::World& world, f32 dt, const Vec3& view);
    // Destroys every physics hair, proxy and settings object this system made (the components and entities stay). Call before the physics world
    // goes and when a level is unloaded or Play stops.
    void clear();

    // The drawn instances of the last tick. Empty with no compute (an inert system draws nothing).
    const HairView* views(u32& count) const {
        count = static_cast<u32>(views_.size());
        return views_.data();
    }

    // ---- ops (the framework relay calls these) ----
    bool teleport(scene::Entity e);                     // the default pose at the next update
    i32 hairOf(scene::Entity e) const;                  // the physics hair handle, 0 when none
    bool isBuilt(scene::Entity e) const;

    // A hull proxy a script adds (Entity.AddHairCollider): a kinematic convex hull on the proxy layer that follows `spec.bone` of the owner's
    // skeleton, so a prop or a held object can catch hair. Applied at the next tick once the groom is built (and again if it is rebuilt); false
    // when the entity carries no CHair or the record has no size. The groom's own proxies are not touched.
    bool addCollider(scene::World& world, scene::Entity e, const fmt::OcHairProxy& spec);
    // Removes every collider a script added to this entity. False when there was none.
    bool clearColliders(scene::Entity e);
    // How many script colliders this entity asked for (including ones still waiting for the build).
    u32 colliderCount(scene::Entity e) const;

    // For scripts and tools: the component's OUT fields plus whether the groom is built and how many script colliders exist.
    struct StateOut {
        bool built = false;
        u32 outState = 0;                   // kHair* bits, level from kHairLodShift
        u32 renderStrands = 0, simStrands = 0;
        u32 colliders = 0;
    };
    bool stateOf(scene::World& world, scene::Entity e, StateOut& out) const;

    // ---- for the editor overlay (Show > Hair) ----
    // The groom's guide strands at rest, hair space, centimetres: a sample of the strands as 2 points (x y z, x y z) per segment, null before the
    // groom is built. Valid until the next tick.
    const std::vector<f32>* guideSegments(scene::Entity e) const;
    struct DebugProxy {
        fmt::OcHairProxyShape shape = fmt::OcHairProxyShape::Sphere;
        f32 sizeCm[3] = {0, 0, 0};
        Mat4 world;                         // the hull body's pose
        bool script = false;
    };
    // The collision proxies of an instance at their live pose. Returns how many were appended.
    u32 debugProxies(scene::Entity e, std::vector<DebugProxy>& out) const;

    HairStats stats() const { return stats_; }

    // ---- the pure parts, public so a test reaches them ----
    // The name of the surface a groom's draw child is interned under.
    static std::string materialNameOf(u64 key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    PathFn pathFn_ = nullptr;
    void* pathUser_ = nullptr;
    HairOptions opt_;
    Vec3 wind_{0, 0, 0};
    f32 gust_ = 0.0f;
    std::vector<HairView> views_;
    HairStats stats_;
};

HairSystem& hairSystem();

// The framework relay's provider (framework_hair_abi.h, aver_fw_hair_fn): `user` is the HairSystem. The signature is spelled without the
// framework header because this module does not link Aver.Framework; the composition root casts it to the pointer type, as it does the soft
// body providers. Ops apply at once (a teleport and a collider are between-step calls); see framework_hair_abi.h for the codes.
i32 hairProvider(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* outF, i32* outI, i32 cap, void* user);

} // namespace aver::hair
