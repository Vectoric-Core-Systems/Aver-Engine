#pragma once
// HairSystem: strand hair on scene entities. Builds each CHair's .ochair groom into Jolt hair settings and publishes a HairView per drawn
// instance for Aver.Render.Hair (spec docs/HAIR.md). No Jolt type appears here; the simulation lives in Aver.Physics.
// Frame order: animSystem().tick, aver_phys_step, World::flush, tick(), then the render side's update and prePass.
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

inline constexpr u32 kHairNoCollision = 0x1;
inline constexpr u32 kHairNoShadow = 0x2;
inline constexpr u32 kHairRayTraced = 0x4;
inline constexpr u32 kHairNoWind = 0x8;
inline constexpr u32 kHairHidden = 0x10;

// CHair::outState bits; the LOD level the groom is drawn at is in bits 8 and up.
inline constexpr u32 kHairSimulating = 0x1;
inline constexpr u32 kHairFrozen = 0x2;         // beyond the cadence's far distance, or not seen for a while
inline constexpr u32 kHairInert = 0x4;          // the build failed or the device has no compute: the component does nothing
inline constexpr u32 kHairBuilding = 0x8;
inline constexpr u32 kHairLodShift = 8;

struct CHair {
    u64 asset = 0;
    u64 material = 0;                // RESERVED: .ocmat override; the groom's LOOK record applies today
    u32 flags = 0;
    f32 widthScale = 0;              // scale fields: 0 reads 1
    f32 densityScale = 0;            // scales the fraction of the render strands drawn
    f32 lodBias = 0;                 // added to the LOD level, in steps
    f32 windScale = 0;
    f32 stiffnessScale = 0;          // divides the bend and stretch compliance of every material
    f32 teleportCm = 0;              // 0 reads HairOptions::teleportCm; a root moving further than this in one frame restarts from the default pose
    u32 reserved = 0;
    u32 outState = 0;                // OUT fields below are written by the system
    u32 outRenderStrands = 0;
    u32 outSimStrands = 0;
    u32 outReserved = 0;
};
static_assert(sizeof(CHair) == 64, "CHair must be padding-free");

// Marks the draw child (entity "Hair") of a CHair; found again through it on load, never made twice.
struct CHairDraw {
    u32 owner = 0;                   // the entity carrying the CHair
    u32 reserved = 0;
};
static_assert(sizeof(CHairDraw) == 8, "CHairDraw must be padding-free");

struct HairOptions {
    i32 hairLayer = 13;              // the layer hair collides as; proxies sit on proxyLayer and touch only this one
    i32 proxyLayer = 12;
    i32 freeLayer = 11;              // a hair with kHairNoCollision collides as this layer, which touches nothing
    f32 teleportCm = 200.0f;
    f32 windDragPerS = 3.0f;
    f32 hideBeyondCm = 6000.0f;
    HairCadenceOptions cadence;
    u32 maxInstances = 8;
    u32 maxBuildsPerFrame = 1;
    u32 vramBudgetMB = 96;           // a groom whose draw buffers would exceed this is refused at build
    u32 cpuMaxSimStrands = 64;       // the Jolt CPU backend is for tests: larger grooms are refused on it
    f32 maxRootToScalpCm = 2.0f;
};

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

// One drawn instance. Pointers belong to the system's per-groom data and stay valid until the next tick().
struct HairView {
    scene::Entity entity = scene::kInvalidEntity;
    scene::Entity drawEntity = scene::kInvalidEntity;   // the child whose mesh is substituted
    u64 key = 0;                                        // instances of one key share static GPU data
    i32 settings = 0;
    i32 hair = 0;
    u64 renderBuffer = 0;                               // host handle of the render positions (0 on the CPU backend)
    u32 renderStrands = 0;
    u32 renderVertices = 0;
    const i32* strandTable = nullptr;                   // {first render vertex, count} per render strand
    const f32* widthRootCm = nullptr;
    const f32* widthTipCm = nullptr;
    u64 seed = 1;
    Mat4 world;
    f32 widthScale = 1.0f;
    f32 densityScale = 1.0f;
    f32 lodBias = 0.0f;
    u32 flags = 0;
    f32 lodDistances[3] = {300.0f, 700.0f, 1500.0f};
    f32 lodFractions[4] = {1.0f, 0.5f, 0.25f, 0.12f};
    f32 distanceCm = 0.0f;
    u32 lodLevel = 0;                                   // strand-count level to draw (HairLod.hpp), with hysteresis and lodBias applied
    HairLookView look;
    i32 materialToken = 0;                              // interned surface name of the draw child's material
    std::string materialName;
    bool visible = true;                                // false: hidden by flag or distance, do not draw or expand
    bool settled = false;                               // at least one step recorded: the render positions mean something
    u64 estimatedBytes = 0;
};

struct HairStats {
    u32 instances = 0, simulating = 0, frozen = 0, inert = 0, groomsCached = 0;
    u32 proxies = 0;
    u32 buildsLastTick = 0;
    u32 updatesLastTick = 0;
};

class HairSystem {
public:
    HairSystem();
    ~HairSystem();
    HairSystem(const HairSystem&) = delete;
    HairSystem& operator=(const HairSystem&) = delete;

    // Idempotent. Returns CHair's type id.
    static u32 registerComponents(scene::World& world);
    static u32 hairType();
    static u32 drawType();
    // Use this, never world.addComponent: storage is zero-filled and runs no constructor. Null when the entity is invalid or already has one.
    static CHair* attachHair(scene::World& world, scene::Entity e, u64 asset);

    // Content-id to absolute-path resolver, supplied by the host. Finds .ochair files and their scalp meshes.
    using PathFn = std::string (*)(u64 objectId, void* user);
    void setPathResolver(PathFn fn, void* user) { pathFn_ = fn; pathUser_ = user; }

    // Chooses the compute host before the first groom: a GPU host passes RhiComputeHost's table; null selects Jolt's CPU backend (tests).
    // False when a groom already exists. Call once, on the composition root's thread, before the first tick.
    bool installComputeHost(const AverPhysComputeHostV1* table);

    HairOptions& options() { return opt_; }
    const HairOptions& options() const { return opt_; }

    // World wind in cm/s, with a gust amplitude in cm/s. Forwarded where SoftBodies.SetWind lands.
    void setWind(const Vec3& windCmS, f32 gustCmS) { wind_ = windCmS; gust_ = gustCmS; }

    // Once per frame, after the physics step and World::flush. `view` is the camera position in world cm.
    void tick(scene::World& world, f32 dt, const Vec3& view);
    // Destroys every physics hair, proxy and settings object (components and entities stay). Call before the physics world goes,
    // on level unload and when Play stops.
    void clear();

    // The drawn instances of the last tick. Empty with no compute (an inert system draws nothing).
    const HairView* views(u32& count) const {
        count = static_cast<u32>(views_.size());
        return views_.data();
    }

    bool teleport(scene::Entity e);                     // the default pose at the next update
    // Floating-origin shift (LARGE_WORLD 2.2.5). Subtracts deltaCm from each previous root position, or the teleportCm rule restarts
    // every groom after a 16 km move. Call after World::shiftRoots.
    void rebase(const Vec3& deltaCm);
    i32 hairOf(scene::Entity e) const;                  // the physics hair handle, 0 when none
    bool isBuilt(scene::Entity e) const;

    // Script hull proxy (Entity.AddHairCollider): a kinematic convex hull on the proxy layer that follows spec.bone. Applied at the next tick
    // once built (and again on rebuild). False when there is no CHair or the record has no size. The groom's own proxies are untouched.
    bool addCollider(scene::World& world, scene::Entity e, const fmt::OcHairProxy& spec);
    // Removes every collider a script added to this entity. False when there was none.
    bool clearColliders(scene::Entity e);
    // Script colliders this entity asked for, including ones waiting for the build.
    u32 colliderCount(scene::Entity e) const;

    struct StateOut {
        bool built = false;
        u32 outState = 0;                   // kHair* bits, level from kHairLodShift
        u32 renderStrands = 0, simStrands = 0;
        u32 colliders = 0;
    };
    bool stateOf(scene::World& world, scene::Entity e, StateOut& out) const;

    // Guide strands at rest, hair space, cm: 2 points (6 floats) per segment. Null before the groom is built. Valid until the next tick.
    const std::vector<f32>* guideSegments(scene::Entity e) const;
    struct DebugProxy {
        fmt::OcHairProxyShape shape = fmt::OcHairProxyShape::Sphere;
        f32 sizeCm[3] = {0, 0, 0};
        Mat4 world;
        bool script = false;
    };
    // The collision proxies of an instance at their live pose. Returns how many were appended.
    u32 debugProxies(scene::Entity e, std::vector<DebugProxy>& out) const;

    HairStats stats() const { return stats_; }

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

// Framework relay provider (aver_fw_hair_fn; `user` is the HairSystem). Spelled without the framework header because this module does not
// link Aver.Framework; the composition root casts it, as it does the soft body providers. Ops apply at once. Op codes: framework_hair_abi.h.
i32 hairProvider(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* outF, i32* outI, i32 cap, void* user);

} // namespace aver::hair
