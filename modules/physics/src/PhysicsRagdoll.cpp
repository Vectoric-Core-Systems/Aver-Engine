// PhysicsRagdoll.cpp -- the ragdoll ABI (physics_ragdoll_abi.h) over Jolt's Ragdoll.
//
// The caller's skeleton has a bone per joint; the Jolt skeleton here has one joint per bone that owns a
// part (capsule radius > 0), parented to the nearest ancestor part. Bones without a part ride rigidly on
// that ancestor, so everything below indexes either bones (the ABI) or parts (Jolt), and the two lists
// are kept side by side in Entry.
//
// Parts are plain registered bodies (World::bodies/byId), so every body-level ABI call works on them.
// Jolt's Ragdoll owns their lifetime, which is why a destroy removes the handles first and lets the
// Ragdoll's destructor destroy the Jolt bodies.
#include "aver/physics/physics_ragdoll_abi.h"
#include "Buoyancy.hpp"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr int   kMaxBones = 512;
constexpr float kMinRadiusCm = 0.01f;
constexpr float kDefaultStrength = 0.5f;
constexpr float kGravityMs2 = 9.81f;
constexpr float kMotorDamping = 1.0f;
// Motor ceilings are multiples of the torque that holds a joint's subtree out horizontally, so the same
// strength means the same thing for a child and a giant. Strength 0.05 is about "just carries it".
constexpr float kTorqueAtFullStrength = 20.0f;
// A little joint friction so a limp ragdoll settles instead of swinging forever.
constexpr float kFrictionOfHold = 0.02f;

struct Entry {
    int32_t id = 0;
    int boneCount = 0;
    std::vector<int>     ownerPart;    // bone -> the part it is or rides on, -1 if none
    std::vector<int>     partBone;     // part -> its bone
    std::vector<int>     partParent;   // part -> parent part, -1 for a root
    std::vector<int32_t> partBody;     // part -> body handle
    std::vector<float>   partHold;     // part -> torque (N*m) that holds its subtree out horizontally
    JPH::Ref<JPH::RagdollSettings> settings;
    JPH::Ref<JPH::Ragdoll> ragdoll;
    int   mode = AVER_PHYS_RAGDOLL_KINEMATIC;
    float strength = kDefaultStrength;
    std::vector<JPH::Vec3> tPos;       // target pose per bone, Jolt space
    std::vector<JPH::Quat> tRot;
};

std::unordered_map<int32_t, Entry> g_ragdolls;
std::unordered_map<int32_t, std::pair<int32_t, int>> g_partOwner;   // body handle -> (ragdoll, bone)
int32_t g_nextRagdoll = 1;   // also each ragdoll's collision GroupID, so two ragdolls never share a filter

int32_t refuse(AbiError e) { setAbiError(e); return 0; }

Entry* findRagdoll(int32_t h) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return nullptr; }
    auto it = g_ragdolls.find(h);
    if (it == g_ragdolls.end()) { setAbiError(AbiError::BadHandle); return nullptr; }
    setAbiError(AbiError::Ok);
    return &it->second;
}

// Inside a step Jolt holds every body lock, so the no-lock interface is the only legal one there.
JPH::BodyInterface& bodiesOf(bool locking) {
    return locking ? g_world->system.GetBodyInterface() : g_world->system.GetBodyInterfaceNoLock();
}

bool allFinite(const float* v, int count) {
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool readPose(int n, const float* pos, const float* rot, std::vector<JPH::Vec3>& p, std::vector<JPH::Quat>& r) {
    if (!allFinite(pos, 3 * n) || !allFinite(rot, 4 * n)) return false;
    p.resize(n);
    r.resize(n);
    for (int i = 0; i < n; ++i) {
        const float* q = rot + 4 * i;
        if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] < 1e-12f) return false;
        p[i] = toJolt(Vec3(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]));
        r[i] = toJolt(Quat(q[0], q[1], q[2], q[3])).Normalized();
    }
    return true;
}

// Moves every part toward its bone's target so it arrives in `dt`.
void driveKinematic(Entry& e, float dt, bool locking) {
    JPH::BodyInterface& b = bodiesOf(locking);
    for (int k = 0; k < static_cast<int>(e.partBone.size()); ++k) {
        const JPH::BodyID id = e.ragdoll->GetBodyID(k);
        // Waking a sleeper changes the active list, which a step has frozen; set_target does it between steps.
        if (!locking && !b.IsActive(id)) continue;
        const int bone = e.partBone[k];
        b.MoveKinematic(id, JPH::RVec3(e.tPos[bone]), e.tRot[bone], dt);
    }
}

template <class F>
void forEachJoint(Entry& e, F&& f) {
    for (int k = 0; k < static_cast<int>(e.partBone.size()); ++k) {
        const int ci = e.settings->GetConstraintIndexForBodyIndex(k);
        if (ci < 0) continue;
        f(k, static_cast<JPH::SwingTwistConstraint*>(e.ragdoll->GetConstraint(ci)));
    }
}

void setMotorsOff(Entry& e) {
    forEachJoint(e, [](int, JPH::SwingTwistConstraint* c) {
        c->SetSwingMotorState(JPH::EMotorState::Off);
        c->SetTwistMotorState(JPH::EMotorState::Off);
    });
}

// Spring frequency and torque ceiling from strength; the ceiling follows what the joint must carry.
void applyMotorSettings(Entry& e) {
    const float freq = 2.0f + 8.0f * e.strength;
    forEachJoint(e, [&](int k, JPH::SwingTwistConstraint* c) {
        JPH::MotorSettings ms(freq, kMotorDamping);
        ms.SetTorqueLimit(e.partHold[k] * kTorqueAtFullStrength * e.strength);
        c->GetSwingMotorSettings() = ms;
        c->GetTwistMotorSettings() = ms;
    });
}

// Each part's orientation relative to its parent part, in the target pose, is the motor target.
void driveMotors(Entry& e) {
    JPH::SkeletonPose pose;
    pose.SetSkeleton(e.settings->GetSkeleton());
    for (int k = 0; k < static_cast<int>(e.partBone.size()); ++k) {
        const int p = e.partParent[k];
        if (p < 0) continue;
        pose.GetJoint(k).mRotation = (e.tRot[e.partBone[p]].Conjugated() * e.tRot[e.partBone[k]]).Normalized();
    }
    e.ragdoll->DriveToPoseUsingMotors(pose);
}

void activateParts(Entry& e) {
    for (JPH::BodyID id : e.ragdoll->GetBodyIDs()) bi().ActivateBody(id);
}

void applyMode(Entry& e, int mode) {
    if (mode == e.mode) return;
    JPH::BodyInterface& b = bi();
    const int prev = e.mode;
    e.mode = mode;

    if (mode == AVER_PHYS_RAGDOLL_KINEMATIC) {
        for (JPH::BodyID id : e.ragdoll->GetBodyIDs()) {
            b.SetMotionType(id, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
            b.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
        forEachJoint(e, [](int, JPH::SwingTwistConstraint* c) {
            c->SetSwingMotorState(JPH::EMotorState::Off);
            c->SetTwistMotorState(JPH::EMotorState::Off);
            c->SetEnabled(false);
        });
        driveKinematic(e, g_world->fixedStep, true);
        return;
    }

    if (prev == AVER_PHYS_RAGDOLL_KINEMATIC) {
        // The parts keep the velocity MoveKinematic last gave them: the animation's momentum.
        for (JPH::BodyID id : e.ragdoll->GetBodyIDs())
            b.SetMotionType(id, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
        forEachJoint(e, [](int, JPH::SwingTwistConstraint* c) { c->SetEnabled(true); });
        e.ragdoll->ResetWarmStart();
    }
    if (mode == AVER_PHYS_RAGDOLL_LIMP) {
        setMotorsOff(e);
    } else {
        applyMotorSettings(e);
        driveMotors(e);
    }
    activateParts(e);
}

// ---- Stepping -------------------------------------------------------------------------------------------

// A kinematic part's velocity is a one-step velocity, so it must be recomputed every step or the part
// keeps drifting after the target stops changing. Registered only while a ragdoll exists: a step
// listener makes Jolt update even when nothing is awake.
class Stepper final : public JPH::PhysicsStepListener {
public:
    void OnStep(const JPH::PhysicsStepListenerContext& ctx) override {
        for (auto& [id, e] : g_ragdolls)
            if (e.mode == AVER_PHYS_RAGDOLL_KINEMATIC) driveKinematic(e, ctx.mDeltaTime, false);
    }
};

Stepper g_stepper;
bool g_stepperAdded = false;

void addStepper() {
    if (g_stepperAdded) return;
    g_world->system.AddStepListener(&g_stepper);
    g_stepperAdded = true;
}

void removeStepper() {
    if (!g_stepperAdded) return;
    if (g_world) g_world->system.RemoveStepListener(&g_stepper);
    g_stepperAdded = false;
}

// ---- Building -------------------------------------------------------------------------------------------

struct PartBuild {
    JPH::Ref<JPH::Shape> shape;
    JPH::Vec3 axisLocal = JPH::Vec3::sAxisX();   // twist axis in the bone's frame, unit
    JPH::Vec3 endA, endB;                        // segment ends in the bone's frame, metres
    float radiusM = 0.0f;
    float volume = 0.0f;
};

bool buildPartShape(const float* c, PartBuild& out) {
    const JPH::Vec3 a = toJolt(Vec3(c[0], c[1], c[2]));
    const JPH::Vec3 b = toJolt(Vec3(c[3], c[4], c[5]));
    const float r = cmToM(c[6]);
    const JPH::Vec3 d = b - a;
    const float len = d.Length();
    out.endA = a;
    out.endB = b;
    out.radiusM = r;

    JPH::Ref<JPH::Shape> inner;
    JPH::Vec3 centre = a;
    JPH::Quat rot = JPH::Quat::sIdentity();
    if (len < 1e-4f) {
        // A segment shorter than a hundredth of a millimetre is a sphere; Jolt's capsule needs a cylinder.
        JPH::SphereShapeSettings s(r);
        s.SetEmbedded();
        auto res = s.Create();
        if (res.HasError()) return false;
        inner = res.Get();
        out.axisLocal = toJoltUnit(Vec3(1.0f, 0.0f, 0.0f));
        out.volume = 4.0f / 3.0f * JPH::JPH_PI * r * r * r;
    } else {
        JPH::CapsuleShapeSettings s(len * 0.5f, r);
        s.SetEmbedded();
        auto res = s.Create();
        if (res.HasError()) return false;
        inner = res.Get();
        centre = (a + b) * 0.5f;
        out.axisLocal = d / len;
        rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), out.axisLocal);
        out.volume = JPH::JPH_PI * r * r * len + 4.0f / 3.0f * JPH::JPH_PI * r * r * r;
    }
    JPH::RotatedTranslatedShapeSettings placed(centre, rot, inner.GetPtr());
    placed.SetEmbedded();
    auto res = placed.Create();
    if (res.HasError()) return false;
    out.shape = res.Get();
    return true;
}

void destroyEntry(int32_t id) {
    auto it = g_ragdolls.find(id);
    if (it == g_ragdolls.end()) return;
    Entry& e = it->second;
    for (int32_t h : e.partBody) {
        phys::water::waterVolumes().clear(h);
        g_partOwner.erase(h);
        if (!g_world) continue;
        auto bit = g_world->bodies.find(h);
        if (bit != g_world->bodies.end()) {
            g_world->byId.erase(bit->second);
            g_world->bodies.erase(bit);
        }
        g_world->sensors.erase(h);
    }
    if (g_world && e.ragdoll) { e.ragdoll->RemoveFromPhysicsSystem(); noteConstraintTopology(); }
    g_ragdolls.erase(it);   // the last reference: the Ragdoll destroys its bodies
    if (g_ragdolls.empty()) removeStepper();
}

} // namespace

// ---- what PhysicsInternal.hpp declares --------------------------------------------------------------------

namespace aver::physics::detail {

void destroyAllRagdolls() {
    std::vector<int32_t> ids;
    ids.reserve(g_ragdolls.size());
    for (const auto& [id, e] : g_ragdolls) ids.push_back(id);
    for (int32_t id : ids) destroyEntry(id);
    removeStepper();
    g_nextRagdoll = 1;   // ids (and collision group ids) restart with the world, like body handles: a rebuilt scene hashes the same
}

void ragdollHandles(std::vector<int32_t>& out) {
    out.clear();
    out.reserve(g_ragdolls.size());
    for (const auto& [id, e] : g_ragdolls) out.push_back(id);
    std::sort(out.begin(), out.end());
}

void releaseRagdollOfBody(int32_t body) {
    auto it = g_partOwner.find(body);
    if (it != g_partOwner.end()) destroyEntry(it->second.first);
}

} // namespace aver::physics::detail

// ---- ABI -----------------------------------------------------------------------------------------------------

extern "C" {

int32_t aver_phys_ragdoll_create(int32_t boneCount, const int32_t* parents,
                                 const float* worldPos, const float* worldRot,
                                 const float* capsules, const float* limitsDeg,
                                 float totalMassKg, uint32_t userLayer) {
    if (!g_world) return refuse(AbiError::NotInitialised);
    if (!parents || !worldPos || !worldRot || !capsules || !limitsDeg) return refuse(AbiError::NullPointer);
    const int n = boneCount;
    if (n < 1 || n > kMaxBones || !std::isfinite(totalMassKg) || totalMassKg <= 0.0f ||
        userLayer >= Layers::kUserLayerCount)
        return refuse(AbiError::InvalidArgument);
    if (!allFinite(capsules, 7 * n) || !allFinite(limitsDeg, 3 * n)) return refuse(AbiError::InvalidArgument);
    for (int i = 0; i < n; ++i) {
        if (parents[i] < -1 || parents[i] >= i) return refuse(AbiError::InvalidArgument);
        const float r = capsules[7 * i + 6];
        if (r > 0.0f && r < kMinRadiusCm) return refuse(AbiError::InvalidArgument);
    }
    std::vector<JPH::Vec3> tp;
    std::vector<JPH::Quat> tr;
    if (!readPose(n, worldPos, worldRot, tp, tr)) return refuse(AbiError::InvalidArgument);

    // Bones to parts.
    std::vector<int> ownerPart(n, -1), partBone, partParent;
    for (int i = 0; i < n; ++i) {
        if (capsules[7 * i + 6] > 0.0f) {
            ownerPart[i] = static_cast<int>(partBone.size());
            partBone.push_back(i);
        } else {
            ownerPart[i] = parents[i] >= 0 ? ownerPart[parents[i]] : -1;
        }
    }
    const int P = static_cast<int>(partBone.size());
    if (P == 0) return refuse(AbiError::InvalidArgument);
    for (int k = 0; k < P; ++k) partParent.push_back(parents[partBone[k]] >= 0 ? ownerPart[parents[partBone[k]]] : -1);

    std::vector<PartBuild> build(P);
    float totalVolume = 0.0f;
    for (int k = 0; k < P; ++k) {
        if (!buildPartShape(capsules + 7 * partBone[k], build[k])) return refuse(AbiError::InvalidArgument);
        totalVolume += build[k].volume;
    }
    std::vector<float> mass(P);
    for (int k = 0; k < P; ++k) mass[k] = totalMassKg * build[k].volume / totalVolume;

    // Hold torque per joint: the subtree's weight at the farthest reach of its capsules, in the build pose.
    std::vector<float> subMass = mass, reach(P, 0.0f), hold(P, 0.0f);
    for (int k = P - 1; k >= 0; --k)
        if (partParent[k] >= 0) subMass[partParent[k]] += subMass[k];
    for (int j = 0; j < P; ++j) {
        const int bj = partBone[j];
        const JPH::Vec3 ea = tp[bj] + tr[bj] * build[j].endA, eb = tp[bj] + tr[bj] * build[j].endB;
        for (int anc = j; anc >= 0; anc = partParent[anc]) {
            const JPH::Vec3 jp = tp[partBone[anc]];
            reach[anc] = std::max({reach[anc], (ea - jp).Length() + build[j].radiusM, (eb - jp).Length() + build[j].radiusM});
        }
    }
    for (int k = 0; k < P; ++k) hold[k] = subMass[k] * kGravityMs2 * reach[k];

    // Settings: one part per part-bone, a swing-twist to the parent part, nothing collides within.
    JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton;
    JPH::Ref<JPH::RagdollSettings> rs = new JPH::RagdollSettings;
    rs->mSkeleton = skeleton;
    rs->mParts.resize(P);
    JPH::Ref<JPH::GroupFilterTable> filter = new JPH::GroupFilterTable(static_cast<JPH::uint>(P));
    for (int a = 0; a < P; ++a)
        for (int b = a + 1; b < P; ++b) filter->DisableCollision(a, b);

    const JPH::ObjectLayer layer = Layers::encode(userLayer, true);
    const float toRad = JPH::JPH_PI / 180.0f;
    for (int k = 0; k < P; ++k) {
        const int bone = partBone[k];
        skeleton->AddJoint("b" + std::to_string(bone), partParent[k]);

        JPH::RagdollSettings::Part& part = rs->mParts[k];
        part.SetShape(build[k].shape.GetPtr());
        part.mPosition = JPH::RVec3(tp[bone]);
        part.mRotation = tr[bone];
        part.mMotionType = JPH::EMotionType::Kinematic;   // a ragdoll starts animated; set_mode lets go
        part.mObjectLayer = layer;
        part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        part.mMassPropertiesOverride.mMass = mass[k];
        part.mAngularDamping = 0.3f;
        part.mCollisionGroup.SetGroupFilter(filter.GetPtr());
        part.mCollisionGroup.SetSubGroupID(static_cast<JPH::uint>(k));

        if (partParent[k] < 0) continue;
        // Twist along the segment; the plane axis is the bone's +Z (or +Y) made perpendicular to it.
        const JPH::Vec3 axis = build[k].axisLocal;
        JPH::Vec3 ref = toJoltUnit(Vec3(0.0f, 0.0f, 1.0f));
        if (std::fabs(axis.Dot(ref)) > 0.99f) ref = toJoltUnit(Vec3(0.0f, 1.0f, 0.0f));
        const JPH::Vec3 planeLocal = (ref - axis * axis.Dot(ref)).Normalized();

        JPH::Ref<JPH::SwingTwistConstraintSettings> c = new JPH::SwingTwistConstraintSettings;
        c->mSpace = JPH::EConstraintSpace::WorldSpace;
        c->mPosition1 = c->mPosition2 = JPH::RVec3(tp[bone]);
        c->mTwistAxis1 = c->mTwistAxis2 = tr[bone] * axis;
        c->mPlaneAxis1 = c->mPlaneAxis2 = tr[bone] * planeLocal;
        c->mSwingType = JPH::ESwingType::Cone;
        const float* lim = limitsDeg + 3 * bone;
        auto half = [&](float deg) { return std::clamp(deg, 0.0f, 180.0f) * toRad; };
        c->mPlaneHalfConeAngle = half(lim[0]);    // swing1: toward the plane axis
        c->mNormalHalfConeAngle = half(lim[1]);   // swing2: toward the normal axis
        c->mTwistMinAngle = -half(lim[2]);
        c->mTwistMaxAngle = half(lim[2]);
        c->mMaxFrictionTorque = hold[k] * kFrictionOfHold;
        part.mToParent = c;
    }
    rs->CalculateBodyIndexToConstraintIndex();
    rs->CalculateConstraintIndexToBodyIdxPair();
    rs->CalculateConstraintPriorities();

    const int32_t id = g_nextRagdoll++;
    JPH::Ref<JPH::Ragdoll> rd = rs->CreateRagdoll(static_cast<JPH::CollisionGroup::GroupID>(id), 0, &g_world->system);
    if (!rd) {
        AVER_WARN("[Physics] ragdoll of {} parts refused: body limit reached", P);
        return refuse(AbiError::AllocationFailed);
    }
    for (int i = 0; i < static_cast<int>(rd->GetConstraintCount()); ++i) rd->GetConstraint(i)->SetEnabled(false);
    rd->AddToPhysicsSystem(JPH::EActivation::Activate);
    noteConstraintTopology();   // the ragdoll's joints are system constraints: a state restore refuses to cross this

    Entry& e = g_ragdolls[id];
    e.id = id;
    e.boneCount = n;
    e.ownerPart = std::move(ownerPart);
    e.partBone = std::move(partBone);
    e.partParent = std::move(partParent);
    e.partHold = std::move(hold);
    e.settings = rs;
    e.ragdoll = rd;
    e.tPos = std::move(tp);
    e.tRot = std::move(tr);
    for (int k = 0; k < P; ++k) {
        const int32_t h = g_world->nextHandle++;
        g_world->bodies.emplace(h, rd->GetBodyID(k));
        g_world->byId.emplace(rd->GetBodyID(k), h);
        e.partBody.push_back(h);
        g_partOwner.emplace(h, std::make_pair(id, e.partBone[k]));
    }
    applyMotorSettings(e);
    addStepper();
    return id;
}

int32_t aver_phys_ragdoll_destroy(int32_t ragdoll) {
    if (!findRagdoll(ragdoll)) return 0;
    destroyEntry(ragdoll);
    return 1;
}

int32_t aver_phys_ragdoll_count(void) {
    return g_world ? static_cast<int32_t>(g_ragdolls.size()) : 0;
}

int32_t aver_phys_ragdoll_bone_count(int32_t ragdoll) {
    Entry* e = findRagdoll(ragdoll);
    return e ? e->boneCount : 0;
}

int32_t aver_phys_ragdoll_set_mode(int32_t ragdoll, int32_t mode) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (mode < AVER_PHYS_RAGDOLL_KINEMATIC || mode > AVER_PHYS_RAGDOLL_POWERED) return refuse(AbiError::InvalidArgument);
    applyMode(*e, mode);
    return 1;
}

int32_t aver_phys_ragdoll_mode(int32_t ragdoll) {
    Entry* e = findRagdoll(ragdoll);
    return e ? e->mode : -1;
}

int32_t aver_phys_ragdoll_set_strength(int32_t ragdoll, float strength) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (!std::isfinite(strength)) return refuse(AbiError::InvalidArgument);
    e->strength = std::clamp(strength, 0.0f, 1.0f);
    if (e->mode == AVER_PHYS_RAGDOLL_POWERED) applyMotorSettings(*e);
    return 1;
}

int32_t aver_phys_ragdoll_set_target(int32_t ragdoll, const float* worldPos, const float* worldRot) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (!worldPos || !worldRot) return refuse(AbiError::NullPointer);
    std::vector<JPH::Vec3> p;
    std::vector<JPH::Quat> r;
    if (!readPose(e->boneCount, worldPos, worldRot, p, r)) return refuse(AbiError::InvalidArgument);

    // A powered ragdoll that is asleep must wake for a new target, but not for the same one every frame.
    bool changed = false;
    for (int i = 0; i < e->boneCount && !changed; ++i)
        changed = !p[i].IsClose(e->tPos[i], 1e-8f) || std::fabs(r[i].Dot(e->tRot[i])) < 0.999999f;
    e->tPos = std::move(p);
    e->tRot = std::move(r);

    if (e->mode == AVER_PHYS_RAGDOLL_KINEMATIC) {
        driveKinematic(*e, g_world->fixedStep, true);
    } else if (e->mode == AVER_PHYS_RAGDOLL_POWERED) {
        driveMotors(*e);
        if (changed) activateParts(*e);
    }
    return 1;
}

int32_t aver_phys_ragdoll_set_pose(int32_t ragdoll, const float* worldPos, const float* worldRot) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (!worldPos || !worldRot) return refuse(AbiError::NullPointer);
    std::vector<JPH::Vec3> p;
    std::vector<JPH::Quat> r;
    if (!readPose(e->boneCount, worldPos, worldRot, p, r)) return refuse(AbiError::InvalidArgument);
    e->tPos = std::move(p);
    e->tRot = std::move(r);

    JPH::BodyInterface& b = bi();
    for (int k = 0; k < static_cast<int>(e->partBone.size()); ++k) {
        const JPH::BodyID id = e->ragdoll->GetBodyID(k);
        const int bone = e->partBone[k];
        b.SetPositionAndRotation(id, JPH::RVec3(e->tPos[bone]), e->tRot[bone], JPH::EActivation::DontActivate);
        b.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }
    e->ragdoll->ResetWarmStart();
    if (e->mode == AVER_PHYS_RAGDOLL_POWERED) driveMotors(*e);
    if (e->mode != AVER_PHYS_RAGDOLL_KINEMATIC) activateParts(*e);
    return 1;
}

int32_t aver_phys_ragdoll_get_pose(int32_t ragdoll, float* outWorldPos, float* outWorldRot) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (!outWorldPos || !outWorldRot) return refuse(AbiError::NullPointer);

    const int P = static_cast<int>(e->partBone.size());
    std::vector<JPH::Vec3> cp(P);
    std::vector<JPH::Quat> cr(P);
    for (int k = 0; k < P; ++k) {
        JPH::RVec3 pos;
        bi().GetPositionAndRotation(e->ragdoll->GetBodyID(k), pos, cr[k]);
        cp[k] = JPH::Vec3(pos);
    }
    for (int i = 0; i < e->boneCount; ++i) {
        JPH::Vec3 pos = e->tPos[i];
        JPH::Quat rot = e->tRot[i];
        const int o = e->ownerPart[i];
        if (o >= 0) {
            const int ob = e->partBone[o];
            if (ob == i) {
                pos = cp[o];
                rot = cr[o];
            } else {
                // The ancestor part's transform times the target pose's offset between the two.
                const JPH::Quat inv = e->tRot[ob].Conjugated();
                pos = cp[o] + cr[o] * (inv * (e->tPos[i] - e->tPos[ob]));
                rot = (cr[o] * (inv * e->tRot[i])).Normalized();
            }
        }
        writeVec(outWorldPos + 3 * i, fromJolt(pos));
        const Quat q = fromJolt(rot);
        outWorldRot[4 * i] = q.x;
        outWorldRot[4 * i + 1] = q.y;
        outWorldRot[4 * i + 2] = q.z;
        outWorldRot[4 * i + 3] = q.w;
    }
    return 1;
}

int32_t aver_phys_ragdoll_add_impulse(int32_t ragdoll, int32_t bone, float ix, float iy, float iz,
                                      float px, float py, float pz) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (bone < 0 || bone >= e->boneCount) return refuse(AbiError::OutOfRange);
    const float v[6] = {ix, iy, iz, px, py, pz};
    if (!allFinite(v, 6)) return refuse(AbiError::InvalidArgument);
    const int o = e->ownerPart[bone];
    if (o < 0 || e->mode == AVER_PHYS_RAGDOLL_KINEMATIC) return 0;
    bi().AddImpulse(e->ragdoll->GetBodyID(o), toJoltDir(Vec3(ix, iy, iz)), toJolt(Vec3(px, py, pz)));
    return 1;
}

int32_t aver_phys_ragdoll_body_of_bone(int32_t ragdoll, int32_t bone) {
    Entry* e = findRagdoll(ragdoll);
    if (!e) return 0;
    if (bone < 0 || bone >= e->boneCount) return refuse(AbiError::OutOfRange);
    const int o = e->ownerPart[bone];
    return o < 0 ? 0 : e->partBody[o];
}

int32_t aver_phys_ragdoll_of_body(int32_t body, int32_t* outBone) {
    if (outBone) *outBone = -1;
    auto it = g_partOwner.find(body);
    if (!g_world || it == g_partOwner.end()) return 0;
    if (outBone) *outBone = it->second.second;
    return it->second.first;
}

} // extern "C"
