// CMotionMatcher: motion matching (docs/ANIM_MOTION_MATCHING.md) on an entity.
//
// REGISTERED DYNAMICALLY, exactly as CAnimGraph is, so Components.hpp, scene_abi.h and the built-in
// component ids stay put. Attach through MotionMatchSystem::attach, never world.addComponent directly: the
// storage arrives zero-filled and runs no constructor. Zero-filled storage is a valid, enabled component
// (play rate 0 reads 1, the character faces its velocity, the database's own defaults apply).
//
// NO C ABI: a script drives the matcher by writing fields (desired velocity, facing, tag hashes, flags)
// and reads the playing clip, cost and switch count back, through the generic scene field ABI.
//
// TWO WAYS TO RUN ONE, never both on an entity: STANDALONE (`database` set, no CAnimGraph instance) the
// system owns a MotionMatcher and is the pose source for the entity; INSIDE A STATE MACHINE (a
// `motionmatch` state in the .ocasm) this system only supplies the trajectory query and the databases,
// and the machine's layer owns the matcher.
#pragma once

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/MotionMatcher.hpp"

#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::anim {

inline constexpr u32 kMotionTagSlots = 4;
inline constexpr u32 kMotionPaused = 0x1, kMotionForceSearch = 0x2, kMotionFaceYaw = 0x4;
// Where the matcher takes its intent from: the desiredVel / desiredYaw fields (a script writes them), or the
// entity's own measured velocity and heading (anything that moves it, with no script).
inline constexpr u32 kMotionInputScript = 0, kMotionInputEntityVelocity = 1;

struct CMotionMatcher {
    u64 database = 0;          // .ocmotion ObjectId; 0 = none (a state-machine motionmatch state names its own)
    u32 flags = 0;             // kMotionPaused; kMotionForceSearch (script sets, system clears); kMotionFaceYaw (else face the velocity)
    f32 playRate = 0.0f;       // 0 reads as 1
    f32 searchInterval = 0.0f; // <= 0 = database default
    f32 desiredVelX = 0.0f, desiredVelY = 0.0f;   // world-space planar velocity the character is trying to reach, cm/s
    f32 desiredYaw = 0.0f;     // radians about +Z, world; used with kMotionFaceYaw
    f32 velHalfLife = 0.0f, turnHalfLife = 0.0f;  // <= 0 = database default
    u64 requireTag[kMotionTagSlots] = {}, excludeTag[kMotionTagSlots] = {};   // fnv1a64(tag name); 0 = unused
    // OUT (written every tick by the system)
    u64 clip = 0;              // fnv1a64 of the playing clip path
    f32 clipTime = 0.0f, cost = 0.0f;
    u32 switches = 0, row = 0;
    f32 sinceSwitch = 0.0f;
    u32 inputSource = 0;       // kMotionInputScript (0, today's behaviour) / kMotionInputEntityVelocity; the latter writes desiredVel/Yaw back each tick
};
static_assert(sizeof(CMotionMatcher) == 136, "CMotionMatcher must be padding-free");

class MotionMatchSystem {
public:
    static u32 registerComponents(scene::World&);
    // Attaches CMotionMatcher with a fresh value over the zero-filled bytes.
    static CMotionMatcher* attach(scene::World&, scene::Entity, u64 databaseAsset);
    // Attaches an empty one (database 0) unless the entity has one: what a state-machine entity gets so a
    // script can drive its trajectory.
    static CMotionMatcher* ensure(scene::World&, scene::Entity);

    // Chains AnimSystem::poseSource() (the state machines) as `prev_`, then installs itself: INSTALL AFTER
    // AnimGraphSystem, which replaces whatever source was there.
    void install(AnimSystem&, scene::World&);
    static void uninstall(AnimSystem&);

    // Per CMotionMatcher: observe the entity's motion, build its query; standalone entities tick their
    // matcher; loading databases are polled. Call BEFORE AnimGraphSystem::tick (the machines read the query).
    void tick(scene::World&, f32 dt);

    // The entity's query for this tick, or null until it has a runtime and a reference database.
    const MotionQuery* query(scene::Entity) const;
    // The database for an asset id: null while it loads or after it failed. Starts the load on first ask.
    const PoseDatabase* database(u64 assetId);
    // The same, on behalf of an entity running a state machine: the first database an entity's states
    // resolve also fixes the time layout of that entity's query.
    const PoseDatabase* databaseFor(scene::Entity, u64 assetId);

    u32 pendingBuilds() const;            // loading / baking in the background (a loading screen may wait on it)
    u32 buildCount() const { return builds_; }   // full builds (not sidecar loads) since clear()
    u32 loadedCount() const;              // databases ready
    u32 searchesLastTick() const { return searchesLast_; }
    void setMaxSearchesPerTick(u32 n) { maxSearches_ = n ? n : 1; }
    void setBakeSidecars(bool on) { bake_ = on; }

    struct Debug {
        Vec3 origin;
        f32 yaw = 0;
        std::vector<Vec3> history, predicted, matched;
        std::vector<f32> predictedYaw;
        Vec3 desiredVel;
        std::string clip;
        f32 clipTime = 0, cost = 0, currentCost = 0;
        u32 switches = 0;
        std::vector<std::string> groupNames;
        std::vector<f32> groupCurrent, groupBest;
    };
    // World-space points for MotionDebugOverlay. False when the entity has no runtime.
    bool debug(scene::Entity, Debug& out) const;
    // Floating-origin shift (LARGE_WORLD 2.2.5): the trajectory history of every ROOT matcher entity -= delta. The history holds
    // the entity's LOCAL position, so a child's is parent-relative and unchanged.
    void rebase(const scene::World& world, const Vec3& delta);

    // Joins any worker first, then drops every runtime and database.
    void clear();

private:
    struct Runtime {
        TrajectoryPredictor traj;
        MotionQuery q;
        MotionMatcher mm;
        const PoseDatabase* refDb = nullptr;   // fixes the query's trajectory times
        const PoseDatabase* boundDb = nullptr;
        const fmt::OcSkeleton* boundSkel = nullptr;
        bool standalone = false;
        u32 stamp = 0;
    };
    struct LoadResult {
        bool ok = false, fromSidecar = false, wrote = false;
        std::unique_ptr<PoseDatabase> db;
        MotionBuildReport report;
        std::string why;
    };
    struct Slot {
        enum class State : u8 { Loading, Ready, Failed } state = State::Loading;
        std::unique_ptr<PoseDatabase> db;
        std::future<LoadResult> fut;
        std::string name;
    };

    static bool sourceThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user);
    void startLoad(u64 id, Slot& slot);
    void finish(u64 id, Slot& slot);
    void poll();

    std::unordered_map<scene::Entity, Runtime> runtimes_;
    std::unordered_map<u64, Slot> dbs_;
    AnimSystem* anim_ = nullptr;
    PoseSourceFn prev_ = nullptr;
    void* prevUser_ = nullptr;
    u32 stamp_ = 0, epoch_ = 0, builds_ = 0, searchesLast_ = 0, maxSearches_ = 4;
    bool bake_ = true, bakeWarned_ = false;
};

MotionMatchSystem& motionMatchSystem();

} // namespace aver::anim
