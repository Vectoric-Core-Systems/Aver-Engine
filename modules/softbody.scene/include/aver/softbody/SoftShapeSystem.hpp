// SoftShapeSystem: jelly and rope on scene entities, over Jolt soft bodies (docs/plans/wave11/SOFTBODY_HAIR.md 2.4).
//
// A COMPANION of SoftBodySystem, in the same module and namespace, not a second path to the same thing: it owns the Jolt bodies
// of CSoftVolume (a voxel tetrahedral lattice inside the entity's mesh, the mesh embedded by barycentric coordinates) and
// CSoftRope (a chain of Cosserat rods drawn as a generated tube), publishes their PieceViews through
// SoftBodySystem::addViewSource, and draws through the unchanged Aver.Render.SoftBody. It reads the host system for the mesh and
// asset sources, the view position, the notify sink and the cloth bodies the grab / pressure / buoyancy calls also reach.
//
// COMPONENTS ARE REGISTERED DYNAMICALLY, the CCloth pattern: attach through attachJelly / attachRope, never world.addComponent
// directly, because storage arrives zero-filled and runs no constructor (zero-filled storage means the useful state: a scale
// of 0 reads 1, a radius of 0 reads the asset's). The entities do NOT carry CSoftBody (the marker SoftBodySystem adopts as a
// legacy soft body); a rope that has no renderer gets one with the engine cube as its placeholder mesh, whose own draw the
// published tube replaces.
//
// ORDER OF A FRAME: aver_phys_step, World::flush, softBodySystem().tick, softShapeSystem().tick, the draw scene's update. The
// shape system builds its bodies at the first tick that has what it needs (a mesh) within a per-frame budget, reads positions
// back each tick, and ends the tick by refreshing the host's view list.
//
// DETERMINISM. Entities are visited in ascending id; a rebuild budget is spent in that order; state is a pure function of the
// components, the asset bytes, the entity transforms, the relay calls in arrival order and the fixed step count.
#pragma once

#include "aver/softbody/Lattice.hpp"
#include "aver/softbody/Rope.hpp"
#include "aver/softbody/SoftBodySystem.hpp"

#include <map>
#include <string>
#include <vector>

namespace aver::softbody {

// ---- components -----------------------------------------------------------------------------------

inline constexpr u32 kVolNoWorld = 0x1;       // negative sense: zero-filled = collides with the world
inline constexpr u32 kVolNoGravity = 0x2;

inline constexpr u32 kRopeNoWorld = 0x1;      // negative sense: zero-filled = collides with the world
inline constexpr u32 kRopeNoStartPin = 0x2;   // the start end is free
inline constexpr u32 kRopeNoEndPin = 0x4;     // the end end is free
inline constexpr u32 kRopeTail = 0x8;         // set by a cut on the new piece: it starts from the cut rope's nodes, not a planned curve

// CSoftVolume::outState and CSoftRope::outState bits.
inline constexpr u32 kShapeAwake = 0x1, kShapeFrozen = 0x2, kShapeOverCap = 0x4, kShapeFailed = 0x8, kShapeCut = 0x10, kShapeAssetStale = 0x20;
inline constexpr u32 kShapeCountShift = 8;    // CSoftRope: bits 8+ hold the segment count

struct CSoftVolume {                  // "CSoftVolume": jelly
    u64 asset = 0;                    // .ocsoft kind "jelly"; 0 = the jelly preset
    u32 preset = 0;                   // SoftPreset (Jelly) when there is no asset
    u32 flags = 0;
    f32 stiffnessScale = 0;           // 0 reads 1 (divides the volume and edge compliance)
    f32 cellCm = 0;                   // 0 = the asset's lattice cell
    f32 gravityScale = 0;             // 0 reads 1
    f32 dampingScale = 0;             // 0 reads 1
    f32 outVolumeRatio = 0;           // OUT: current / rest volume
    f32 outMaxStretch = 0;            // OUT: the largest lattice edge strain
    u32 outState = 0;                 // OUT: kShape* bits
    u32 outNodes = 0;                 // OUT: lattice node count
};
static_assert(sizeof(CSoftVolume) == 48, "CSoftVolume must be padding-free");

struct CSoftRope {                    // "CSoftRope": rope and cable
    u64 asset = 0;                    // .ocsoft kind "rope"; 0 = the rope preset
    u32 startEntity = 0, endEntity = 0;           // 0 = a world point
    f32 startOffset[3] = {0, 0, 0}, endOffset[3] = {0, 0, 0};   // cm, in the anchor entity's space (world space when the entity is 0)
    f32 lengthCm = 0;                 // 0 = (the distance between the ends) x slack
    f32 radiusCm = 0;                 // 0 = the asset's; the drawn tube
    u32 flags = 0;
    f32 outLengthCm = 0;              // OUT: the current polyline length
    f32 outStrain = 0;                // OUT: (current - rest) / rest
    u32 outState = 0;                 // OUT: kShape* bits, the segment count from bit 8
};
static_assert(sizeof(CSoftRope) == 64, "CSoftRope must be padding-free");

// ---- options, events ------------------------------------------------------------------------------

struct SoftShapeOptions {
    u32 maxRopeNodes = 4000;
    u32 maxVolumeNodes = 8000;
    f32 freezeDistanceCm = 6000.0f;   // beyond this from the view a shape stops simulating
    u32 maxBuildsPerFrame = 2;
    i32 shapeLayer = 14;              // the cloth layer: shapes obey the same layer matrix as cloth
    i32 noWorldLayer = 15;            // kVolNoWorld / kRopeNoWorld: the cloth proxy layer
};

// A soft body entering or leaving a sensor. `sensor` is 0 when the sensor body belongs to no rigid-body component.
struct SoftSensorEvent {
    scene::Entity soft = scene::kInvalidEntity;
    scene::Entity sensor = scene::kInvalidEntity;
    bool entered = false;
};

struct SoftShapeStats {
    u32 volumes = 0, ropes = 0, nodes = 0, awake = 0, frozen = 0, failed = 0;
    u32 buildsLastTick = 0;
};

// ---- the system -----------------------------------------------------------------------------------

class SoftShapeSystem {
public:
    // `host` is the SoftBodySystem whose sources and view list this uses; null = the process singleton.
    explicit SoftShapeSystem(SoftBodySystem* host = nullptr);
    ~SoftShapeSystem();
    SoftShapeSystem(const SoftShapeSystem&) = delete;
    SoftShapeSystem& operator=(const SoftShapeSystem&) = delete;

    // Registers CSoftVolume and CSoftRope (idempotent) and returns CSoftVolume's type id.
    static u32 registerComponents(scene::World& world);
    static u32 volumeType();
    static u32 ropeType();
    // Attach with defaults over the zero-filled bytes. Null when the entity is invalid, has no mesh renderer (jelly), or already
    // carries the other shape component (one body per entity).
    static CSoftVolume* attachJelly(scene::World& world, scene::Entity e, SoftPreset preset = SoftPreset::Jelly, u64 asset = 0);
    // A rope between two world points; the ends are attached to nothing and pinned. lengthCm / radiusCm 0 = from the asset.
    static CSoftRope* attachRope(scene::World& world, scene::Entity e, const Vec3& start, const Vec3& end, u64 asset = 0,
                                 f32 lengthCm = 0.0f, f32 radiusCm = 0.0f);

    // Binds the world and publishes this system's views through the host. tick() calls it when needed.
    void install(scene::World& world);
    void uninstall();
    // Once per frame after the physics step, World::flush and the host system's tick.
    void tick(scene::World& world, f32 dt);
    // Destroys every body and entry (the entities and components stay); keeps the options.
    void clear();

    SoftShapeOptions& options() { return opt_; }
    const SoftShapeOptions& options() const { return opt_; }
    SoftShapeStats stats() const { return stats_; }

    // ---- ops: also the framework relay's calls (softShapeProvider). They work on jelly, rope and, where noted, cloth. ----
    // Entities the system built a body for; false for a stale or failed one.
    bool isShape(scene::Entity e) const;
    // The physics handle behind an entity's soft body (jelly, rope or cloth), 0 when none.
    i32 bodyOf(scene::Entity e) const;
    bool setPressure(scene::Entity e, f32 kPa);                                 // jelly (and cloth, through the host)
    bool volumeOf(scene::Entity e, f32& restCm3, f32& nowCm3) const;
    // The nearest vertex of the entity's body within maxDistanceCm (<= 0: any) is attached to `target` and followed; returns
    // the grab id (vertex index + 1), 0 when nothing is in reach.
    i32 grab(scene::Entity e, const Vec3& point, f32 maxDistanceCm, const Vec3& target);
    bool moveGrab(scene::Entity e, i32 grab, const Vec3& target);
    bool releaseGrab(scene::Entity e, i32 grab);
    bool setBuoyancy(scene::Entity e, f32 fluidDensityKgM3, f32 drag);
    bool submerged(scene::Entity e, f32& fraction, f32& volumeCm3) const;
    bool setContact(scene::Entity e, f32 softScale, f32 otherScale);
    // mode: AVER_PHYS_SBX_PAIR_* (0 normal, 1 sensor, 2 ignore). `other` is a rigid body entity or another soft body entity.
    bool setPairMode(scene::Entity e, scene::Entity other, i32 mode);
    // Splits the rope at its node nearest `point` (an interior node): the entity keeps the start, a NEW entity gets the rest
    // (returned in `other` at once; the bodies are rebuilt at the next tick), both cut ends free. False for a rope that is not
    // built, has fewer than 4 nodes, or has no interior node near the point.
    bool cutRope(scene::World& world, scene::Entity rope, const Vec3& point, scene::Entity& other);
    // Creates an entity carrying a rope between two world points (the relay's createrope). 0 on failure.
    scene::Entity createRope(scene::World& world, const Vec3& start, const Vec3& end, f32 lengthCm, f32 radiusCm, u64 asset = 0);
    // Sensor events of the last tick, oldest first; they are consumed.
    u32 drainSensorEvents(SoftSensorEvent* out, u32 max);

    // ---- for the debug overlay and the gizmos (valid until the next tick) ----
    bool ropeNodes(scene::Entity e, const Vec3*& nodes, u32& count) const;
    bool volumeLattice(scene::Entity e, const Lattice*& lattice, const Vec3*& nodeWorld) const;
    // The rest length of each lattice edge in world cm (one per Lattice::edges pair), for the stretch colouring.
    bool volumeEdgeRest(scene::Entity e, const f32*& rest, u32& count) const;
    // Which vertex indices of a rope are anchored (first, last); false when not attached.
    bool ropeAnchors(scene::Entity e, bool& startAttached, bool& endAttached) const;
    // The world orientation of each rod (nodes - 1 of them, the +X axis along the rod) as of the last tick; false before the first read.
    bool ropeRods(scene::Entity e, const Quat*& rotations, u32& count) const;

private:
    struct VolumeSpec {
        u64 meshId = 0, assetId = 0;
        u32 preset = 0, flags = 0;
        f32 stiffness = 1.0f, cell = 0.0f, gravity = 1.0f, damping = 1.0f;
        bool operator==(const VolumeSpec& o) const {
            return meshId == o.meshId && assetId == o.assetId && preset == o.preset && flags == o.flags && stiffness == o.stiffness &&
                   cell == o.cell && gravity == o.gravity && damping == o.damping;
        }
    };
    struct RopeSpecKey {
        u64 assetId = 0;
        f32 lengthCm = 0.0f, radiusCm = 0.0f;
        u32 flags = 0;                  // kRopeNoWorld only: the pins change live
        bool operator==(const RopeSpecKey& o) const {
            return assetId == o.assetId && lengthCm == o.lengthCm && radiusCm == o.radiusCm && flags == o.flags;
        }
    };

    struct VolumeEntry {
        scene::Entity entity = scene::kInvalidEntity;
        VolumeSpec spec;
        SoftAsset asset;
        bool failed = false, overCap = false, assetStale = false;
        const fmt::OcMeshData* mesh = nullptr;
        Lattice lattice;
        Embedding emb;
        i32 body = 0;
        std::vector<Vec3> nodeWorld, renderWorld;
        std::vector<f32> posBuf;
        std::vector<u32> indices;       // a copy of the mesh's triangles: the host's mesh cache may be cleared under a published view
        std::vector<f32> edgeRest;      // rest length of each lattice edge, world cm (the stretch readout)
        std::vector<u32> sourceVertex;
        Vec3 centroid{0, 0, 0};
        bool frozen = false, needsRead = true, needsPack = true, awake = false;
        u32 epoch = 1;
    };

    struct Anchor {
        bool attached = false;
        bool hasTarget = false;
        Vec3 target{0, 0, 0};
    };

    struct RopeEntry {
        scene::Entity entity = scene::kInvalidEntity;
        RopeSpecKey spec;
        SoftAsset asset;
        bool failed = false, overCap = false, assetStale = false, cutMade = false;
        RopeBuild plan;
        i32 body = 0;
        u32 nodes = 0;
        f32 radiusCm = 1.0f;
        u32 sides = 8;
        f32 freeInvMass = 1.0f;
        f32 restLengthCm = 0.0f;
        std::vector<i32> rodSlots;
        bool slotsIdentity = true;
        std::vector<Vec3> nodeWorld;
        std::vector<Quat> rodRot;
        bool haveRods = false;
        std::vector<f32> posBuf, rodBuf;
        std::vector<Vec3> tubePos;
        std::vector<f32> tubeUv;
        std::vector<u32> tubeIdx, sourceVertex;
        Anchor anchor[2];
        Vec3 centroid{0, 0, 0};
        bool frozen = false, needsRead = true, needsPack = true;
        u32 epoch = 1;
    };

    // A cut waiting for its tick: the head keeps nodes [0, node], the tail (a new entity) takes [node, end].
    struct CutRequest {
        scene::Entity head = scene::kInvalidEntity, tail = scene::kInvalidEntity;
        u32 node = 0;
    };
    struct Seed {
        std::vector<Vec3> pos, vel;
    };

    SoftBodySystem& host() const { return host_ ? *host_ : softBodySystem(); }
    void ensureInstalled(scene::World& world);
    static void viewThunk(std::vector<PieceView>& out, void* user);
    void appendViews(std::vector<PieceView>& out) const;
    void sync(scene::World& world);
    void dropAll(scene::World& world);
    void drainSensors(scene::World& world);
    scene::Entity entityOfAnyBody(scene::World& world, i32 body) const;

    // ---- SoftShapeVolume.cpp ----
    enum class Built { Ready, NotReady, Failed };
    Built buildVolume(scene::World& world, VolumeEntry& v);
    void updateVolume(scene::World& world, VolumeEntry& v);
    void destroyVolume(VolumeEntry& v);
    void writeVolumeOut(scene::World& world, VolumeEntry& v);

    // ---- SoftShapeRope.cpp ----
    Built buildRope(scene::World& world, RopeEntry& r);
    void updateRope(scene::World& world, RopeEntry& r, f32 dt);
    void destroyRope(RopeEntry& r);
    void writeRopeOut(scene::World& world, RopeEntry& r);
    void updateAnchors(scene::World& world, RopeEntry& r);
    void processCuts(scene::World& world);
    bool createRopeBody(scene::World& world, RopeEntry& r, const std::vector<Vec3>& rest, const std::vector<Vec3>* vel, bool seeded);

    SoftBodySystem* host_ = nullptr;
    scene::World* world_ = nullptr;
    SoftShapeOptions opt_;
    SoftShapeStats stats_;
    bool installed_ = false;
    u32 buildsThisTick_ = 0;
    std::map<scene::Entity, VolumeEntry> volumes_;
    std::map<scene::Entity, RopeEntry> ropes_;
    std::map<scene::Entity, Seed> seeds_;              // a cut rope's pieces, until their entries are built
    std::vector<CutRequest> cuts_;
    std::vector<SoftSensorEvent> sensorEvents_;
};

SoftShapeSystem& softShapeSystem();   // process singleton, like softBodySystem()

// fnv1a64("Meshes/cube.ocmesh"): the engine cube, the placeholder mesh of a rope and the id its views carry.
u64 shapeRopeMeshId();

// The framework relay's provider (framework_softshape_abi.h, aver_fw_set_softshape_provider), with the SoftShapeSystem as `user`.
// Declared here so the system need not link the framework: the composition root hands it across:
// aver_fw_set_softshape_provider(&softbody::softShapeProvider, &softbody::softShapeSystem()).
i32 softShapeProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

} // namespace aver::softbody
