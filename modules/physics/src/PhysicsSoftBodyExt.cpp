// PhysicsSoftBodyExt.cpp -- the rest of Jolt's soft bodies (physics_softbody_ext_abi.h): tetrahedral volumes, long
// range attachments, Cosserat rods, Jolt's own constraint generator, pressure, grab and release, contact policy,
// sensors, a collision group and the live setters. Buoyancy's per-step force is SoftBodyFluid.cpp.
//
// ITS OWN TRANSLATION UNIT over the registry in PhysicsSoftBody.cpp (SoftBodyInternal.hpp): finish() calls the
// sbx* hooks defined here, and a body that never calls an aver_phys_sbx_* function keeps a default SoftExt, which
// every hook treats as "do nothing". The builder side stages arrays keyed by the builder id and applies them from
// sbxPrepare (before Optimize) and sbxApply (once the body exists).
//
// THE KINEMATIC TRICK. Jolt says a vertex' inverse mass and velocity are the only things to touch at run time
// (SoftBodyVertex.h). A vertex with inverse mass 0 is moved by its own velocity (IntegratePositions runs for every
// vertex; the XPBD velocity update skips kinematic ones), so an attachment is: inverse mass 0, and at the start of
// every fixed step a velocity that carries the vertex to its target in one step. Frame-rate independent because the
// step is the fixed one.
#include "aver/physics/physics_softbody_ext_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "SoftBodyInternal.hpp"
#include "PhysicsOrigin.hpp"   // LARGE_WORLD: physicsOriginHook

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Trigonometry.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <functional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;
using namespace aver::physics::softbody;

namespace {

constexpr int kP = AVER_PHYS_SBX_P_COUNT;

int32_t refuse(AbiError e) {
    setAbiError(e);
    return 0;
}

int32_t succeed() {
    setAbiError(AbiError::Ok);
    return 1;
}

// What a builder staged through the sbx calls, until finish or cancel.
struct BuilderExt {
    std::vector<int32_t> tets;                  // 4 per tetrahedron
    std::vector<float> tetCompliance;
    std::vector<int32_t> lra;                   // 2 per explicit pair
    std::vector<float> lraMaxCm;
    int32_t lraAutoMode = 0;
    float lraAutoMult = 1.0f;
    std::vector<int32_t> rodAb;                 // 2 per rod
    std::vector<float> rodStretch;
    std::vector<int32_t> pairAb;                // 2 per bend-twist pair, GLOBAL rod indices
    std::vector<float> pairCompliance;
    bool autoStaged = false;
    std::vector<float> attrs;                   // 5 per vertex attribute
    int32_t autoBend = 0;
    float autoAngleDeg = 8.0f;
    float params[kP];
    BuilderExt() { for (float& f : params) f = std::nanf(""); }
    bool any() const { return !tets.empty() || !rodAb.empty() || autoStaged; }
};

std::unordered_map<int32_t, BuilderExt> g_ext;

struct SensorEvent { int32_t soft = 0, sensor = 0, entered = 0; };
std::vector<SensorEvent> g_sensorEvents;

// ONE FILTER FOR EVERY GROUPED SOFT BODY. Jolt asks the soft body's filter first (CollisionGroup::CanCollide), and a
// GroupFilterTable answers "same group id, different filter object: do not collide", "different group id: collide".
// A ragdoll part carries its ragdoll's own table with group id = the ragdoll handle, so a soft body whose group is
// that handle ignores exactly those parts, and every body with another (or no) group collides as before.
JPH::Ref<JPH::GroupFilterTable> g_groupFilter;

const JPH::GroupFilter* groupFilter() {
    if (!g_groupFilter) g_groupFilter = new JPH::GroupFilterTable(1);
    return g_groupFilter.GetPtr();
}

JPH::CollisionGroup groupOf(int32_t g) {
    return g > 0 ? JPH::CollisionGroup(groupFilter(), static_cast<JPH::CollisionGroup::GroupID>(g), 0) : JPH::CollisionGroup();
}

BuilderExt* stage(int32_t b) {
    if (builderVertexCount(b) == 0) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &g_ext[b];
}

bool finiteNonNeg(float v) { return std::isfinite(v) && v >= 0.0f; }

// Every face edge shared by exactly two faces, and the signed rest volume from the settings' rest positions (Jolt
// space, metres). Positive means the winding points outward, the sign ApplyPressure needs.
void analyse(const JPH::SoftBodySharedSettings& s, bool& closed, float& restVolumeM3) {
    std::vector<uint64_t> keys;
    keys.reserve(s.mFaces.size() * 3);
    double six = 0.0;
    for (const JPH::SoftBodySharedSettings::Face& f : s.mFaces) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = f.mVertex[k], b = f.mVertex[(k + 1) % 3];
            keys.push_back((static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b));
        }
        const JPH::Vec3 x1(s.mVertices[f.mVertex[0]].mPosition), x2(s.mVertices[f.mVertex[1]].mPosition),
            x3(s.mVertices[f.mVertex[2]].mPosition);
        six += static_cast<double>(x1.Cross(x2).Dot(x3));
    }
    std::sort(keys.begin(), keys.end());
    closed = !keys.empty();
    for (size_t i = 0; i < keys.size() && closed;) {
        size_t j = i;
        while (j < keys.size() && keys[j] == keys[i]) ++j;
        if (j - i != 2) closed = false;
        i = j;
    }
    restVolumeM3 = static_cast<float>(six / 6.0);
}

// closed and restVolumeM3, measured the first time something needs them (pressure, volume).
void ensureAnalysed(SoftRecord& r) {
    if (r.ext.analysed) return;
    analyse(*r.settings, r.ext.closed, r.ext.restVolumeM3);
    r.ext.analysed = true;
}

// LRA from the closest kinematic vertex over the staged edges and rods (Jolt's own search is private). Euclidean
// uses the straight distance in the rest pose, geodesic the path along the edges and rods.
void addAutoLra(JPH::SoftBodySharedSettings& s, int32_t mode, float mult) {
    const uint32_t n = static_cast<uint32_t>(s.mVertices.size());
    std::vector<std::vector<uint32_t>> adj(n);
    for (const auto& e : s.mEdgeConstraints) {
        adj[e.mVertex[0]].push_back(e.mVertex[1]);
        adj[e.mVertex[1]].push_back(e.mVertex[0]);
    }
    for (const auto& r : s.mRodStretchShearConstraints) {
        adj[r.mVertex[0]].push_back(r.mVertex[1]);
        adj[r.mVertex[1]].push_back(r.mVertex[0]);
    }
    auto pos = [&](uint32_t i) { return JPH::Vec3(s.mVertices[i].mPosition); };
    std::vector<float> dist(n, FLT_MAX);
    std::vector<uint32_t> src(n, ~0u);
    using QE = std::pair<float, uint32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    for (uint32_t v = 0; v < n; ++v)
        if (s.mVertices[v].mInvMass == 0.0f) { dist[v] = 0.0f; src[v] = v; open.push({0.0f, v}); }
    while (!open.empty()) {
        const auto [d, v] = open.top();
        open.pop();
        if (d > dist[v]) continue;
        for (const uint32_t u : adj[v]) {
            const float nd = d + (pos(u) - pos(v)).Length();
            if (nd < dist[u]) { dist[u] = nd; src[u] = src[v]; open.push({nd, u}); }
        }
    }
    for (uint32_t v = 0; v < n; ++v) {
        if (!(s.mVertices[v].mInvMass > 0.0f) || src[v] == ~0u) continue;
        const float d = (mode == 1 ? (pos(src[v]) - pos(v)).Length() : dist[v]) * mult;
        if (d > 1.0e-6f) s.mLRAConstraints.push_back(JPH::SoftBodySharedSettings::LRA(src[v], v, d));
    }
}

// The attachment of a vertex, or null.
SoftAttachment* attachmentOf(SoftExt& e, int32_t vertex) {
    for (SoftAttachment& a : e.attachments)
        if (a.vertex == vertex) return &a;
    return nullptr;
}

void zeroVelocity(JPH::SoftBodyMotionProperties& mp, int32_t vertex) {
    mp.GetVertices()[static_cast<size_t>(vertex)].mVelocity = JPH::Vec3::sZero();
}

// The body and record behind a handle for a live call, or null (the reason is recorded).
SoftRecord* liveRecord(int32_t body, const JPH::BodyID*& id) {
    id = findBody(body);
    if (!id) return nullptr;
    SoftRecord* r = recordOf(body);
    if (!r) { setAbiError(AbiError::BadHandle); return nullptr; }
    return r;
}

void wake(SoftRecord& r, const JPH::BodyID& id) {
    r.frozen = false;
    g_world->system.GetBodyInterfaceNoLock().ActivateBody(id);   // the caller holds the body lock: see aver_phys_sb_impulse
}

int32_t attachImpl(int32_t body, int32_t vertex, int32_t otherBody, const float* v3, float maxSpeedCmS) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!v3) return refuse(AbiError::NullPointer);
    if (vertex < 0 || vertex >= r->vertexCount) return refuse(AbiError::OutOfRange);
    if (!std::isfinite(v3[0]) || !std::isfinite(v3[1]) || !std::isfinite(v3[2]) || std::isnan(maxSpeedCmS)) return refuse(AbiError::InvalidArgument);
    if (otherBody != 0 && (otherBody == body || !findBody(otherBody))) return otherBody == body ? refuse(AbiError::InvalidArgument) : 0;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    SoftAttachment* a = attachmentOf(r->ext, vertex);
    if (!a) {
        SoftAttachment fresh;
        fresh.vertex = vertex;
        fresh.savedInvMass = mp->GetVertices()[static_cast<size_t>(vertex)].mInvMass;
        r->ext.attachments.push_back(fresh);
        a = &r->ext.attachments.back();
        sbxSetVertexInvMass(*r, *mp, vertex, 0.0f);
        zeroVelocity(*mp, vertex);
        sbxRefreshRodInvMass(*r, *mp);
        mp->CalculateMassAndInertia();
    }
    a->otherBody = otherBody;
    a->targetCm = aver::Vec3(v3[0], v3[1], v3[2]);
    a->maxSpeedCmS = maxSpeedCmS > 0.0f ? maxSpeedCmS : 1.0e6f;
    wake(*r, *id);
    return succeed();
}

} // namespace

// ---- the hooks PhysicsSoftBody.cpp calls -------------------------------------------------------------------

namespace aver::physics::softbody {

bool sbxFacelessOk(int32_t b) {
    const auto it = g_ext.find(b);
    return it != g_ext.end() && it->second.any();
}

AbiError sbxPrepare(int32_t b, JPH::SoftBodySharedSettings& shared, bool stagedEdgesOrBends) {
    const auto it = g_ext.find(b);
    if (it == g_ext.end()) return AbiError::Ok;
    const BuilderExt& E = it->second;
    const uint32_t n = static_cast<uint32_t>(shared.mVertices.size());

    // Jolt's generator first: it replaces the edge list, so a builder that staged edges or bends is refused.
    if (E.autoStaged) {
        if (stagedEdgesOrBends || shared.mFaces.empty()) return AbiError::InvalidArgument;
        std::vector<JPH::SoftBodySharedSettings::VertexAttributes> attrs(E.attrs.size() / 5);
        for (size_t i = 0; i < attrs.size(); ++i) {
            const float* a = &E.attrs[i * 5];
            JPH::SoftBodySharedSettings::VertexAttributes& v = attrs[i];
            v.mCompliance = std::max(0.0f, a[0]);
            v.mShearCompliance = std::max(0.0f, a[1]);
            v.mBendCompliance = a[2] < 0.0f ? FLT_MAX : a[2];
            v.mLRAType = static_cast<JPH::SoftBodySharedSettings::ELRAType>(std::clamp(static_cast<int>(std::lround(a[3])), 0, 2));
            v.mLRAMaxDistanceMultiplier = a[4] > 0.0f ? a[4] : 1.0f;
        }
        shared.CreateConstraints(attrs.data(), static_cast<JPH::uint>(attrs.size()),
                                 static_cast<JPH::SoftBodySharedSettings::EBendType>(std::clamp(E.autoBend, 0, 2)),
                                 JPH::DegreesToRadians(E.autoAngleDeg));
    }

    // Jolt's volume gradient is that of the signed volume, so a tetrahedron wound the other way is pushed the wrong way
    // and blows up: swap two vertices of a negatively oriented one.
    for (size_t t = 0; t + 3 < E.tets.size(); t += 4) {
        uint32_t q[4] = {static_cast<uint32_t>(E.tets[t]), static_cast<uint32_t>(E.tets[t + 1]), static_cast<uint32_t>(E.tets[t + 2]),
                         static_cast<uint32_t>(E.tets[t + 3])};
        if (q[0] >= n || q[1] >= n || q[2] >= n || q[3] >= n) return AbiError::OutOfRange;
        const JPH::Vec3 x1(shared.mVertices[q[0]].mPosition);
        if ((JPH::Vec3(shared.mVertices[q[1]].mPosition) - x1).Cross(JPH::Vec3(shared.mVertices[q[2]].mPosition) - x1)
                .Dot(JPH::Vec3(shared.mVertices[q[3]].mPosition) - x1) < 0.0f)
            std::swap(q[2], q[3]);
        shared.mVolumeConstraints.push_back(JPH::SoftBodySharedSettings::Volume(q[0], q[1], q[2], q[3], E.tetCompliance[t / 4]));
    }
    if (!E.tets.empty()) shared.CalculateVolumeConstraintVolumes();

    if (!E.rodAb.empty()) {
        for (size_t r = 0; r + 1 < E.rodAb.size(); r += 2) {
            const uint32_t a = static_cast<uint32_t>(E.rodAb[r]), c = static_cast<uint32_t>(E.rodAb[r + 1]);
            if (a >= n || c >= n) return AbiError::OutOfRange;
            // A zero-length rod has no direction (Jolt asserts on it).
            if ((JPH::Vec3(shared.mVertices[c].mPosition) - JPH::Vec3(shared.mVertices[a].mPosition)).Length() < 1.0e-6f)
                return AbiError::InvalidArgument;
            shared.mRodStretchShearConstraints.push_back(JPH::SoftBodySharedSettings::RodStretchShear(a, c, E.rodStretch[r / 2]));
        }
        for (size_t p = 0; p + 1 < E.pairAb.size(); p += 2)
            shared.mRodBendTwistConstraints.push_back(JPH::SoftBodySharedSettings::RodBendTwist(
                static_cast<uint32_t>(E.pairAb[p]), static_cast<uint32_t>(E.pairAb[p + 1]), E.pairCompliance[p / 2]));
        shared.CalculateRodProperties();   // Bishop frames, lengths, inverse masses; may swap a rod's vertices
    }

    for (size_t p = 0; p + 1 < E.lra.size(); p += 2)
        shared.mLRAConstraints.push_back(JPH::SoftBodySharedSettings::LRA(
            static_cast<uint32_t>(E.lra[p]), static_cast<uint32_t>(E.lra[p + 1]), cmToM(E.lraMaxCm[p / 2])));
    if (E.lraAutoMode == 1 || E.lraAutoMode == 2) addAutoLra(shared, E.lraAutoMode, E.lraAutoMult);

    // Jolt reads GetFaces().size() - 1 for the sub shape id bits, so a body needs one face. A rod body gets the
    // triangle of its first three vertices; it is one more sliver on a body that collides through its vertices.
    if (shared.mFaces.empty()) {
        if (n < 3) return AbiError::InvalidArgument;
        shared.AddFace(JPH::SoftBodySharedSettings::Face(0, 1, 2));
    }
    return AbiError::Ok;
}

void sbxCarry(const SoftRecord& old, SoftRecord& next) {
    next.ext = old.ext;
    next.ext.analysed = false;
    next.ext.closed = false;
    next.ext.restVolumeM3 = 0.0f;
    next.ext.volumeSlot.clear();
    next.ext.lraSlot.clear();
    next.ext.rodSlot.clear();
    next.ext.rodPairSlot.clear();
}

void sbxApply(int32_t b, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, SoftRecord& rec,
              const JPH::SoftBodySharedSettings::OptimizationResults& results) {
    SoftExt& e = rec.ext;

    // The builder's params over the carried (or default) values.
    if (const auto it = g_ext.find(b); it != g_ext.end()) {
        const float* p = it->second.params;
        auto has = [&](int i) { return std::isfinite(p[i]); };
        if (has(AVER_PHYS_SBX_P_PRESSURE_KPA)) e.pressureKPa = std::max(0.0f, p[AVER_PHYS_SBX_P_PRESSURE_KPA]);
        if (has(AVER_PHYS_SBX_P_SKIN_ENABLED)) e.skinEnabled = p[AVER_PHYS_SBX_P_SKIN_ENABLED] >= 0.5f;
        if (has(AVER_PHYS_SBX_P_SKIN_DISTANCE_SCALE)) e.skinScale = std::max(0.0f, p[AVER_PHYS_SBX_P_SKIN_DISTANCE_SCALE]);
        if (has(AVER_PHYS_SBX_P_COLLISION_GROUP)) e.group = std::max(0, static_cast<int32_t>(std::lround(p[AVER_PHYS_SBX_P_COLLISION_GROUP])));
        if (has(AVER_PHYS_SBX_P_SOFT_INV_MASS_SCALE)) e.softScale = std::max(0.0f, p[AVER_PHYS_SBX_P_SOFT_INV_MASS_SCALE]);
        if (has(AVER_PHYS_SBX_P_OTHER_INV_MASS_SCALE)) e.otherScale = std::max(0.0f, p[AVER_PHYS_SBX_P_OTHER_INV_MASS_SCALE]);
        if (has(AVER_PHYS_SBX_P_OTHER_INV_INERTIA_SCALE)) e.inertiaScale = std::max(0.0f, p[AVER_PHYS_SBX_P_OTHER_INV_INERTIA_SCALE]);
        if (has(AVER_PHYS_SBX_P_FLUID_DENSITY)) e.fluidDensity = std::max(0.0f, p[AVER_PHYS_SBX_P_FLUID_DENSITY]);
        if (has(AVER_PHYS_SBX_P_FLUID_DRAG)) e.fluidDrag = std::max(0.0f, p[AVER_PHYS_SBX_P_FLUID_DRAG]);
        if (has(AVER_PHYS_SBX_P_BUOY_FLAGS)) e.buoyFlags = static_cast<int32_t>(std::lround(p[AVER_PHYS_SBX_P_BUOY_FLAGS]));
    }

    // Where Optimize put each staged item.
    auto copy = [](std::vector<int32_t>& dst, const JPH::Array<JPH::uint>& src) {
        dst.assign(src.size(), 0);
        for (size_t i = 0; i < src.size(); ++i) dst[i] = static_cast<int32_t>(src[i]);
    };
    copy(e.volumeSlot, results.mVolumeRemap);
    copy(e.lraSlot, results.mLRARemap);
    copy(e.rodSlot, results.mRodStretchShearConstraintRemap);
    copy(e.rodPairSlot, results.mRodBendTwistConstraintRemap);

    // Closed and the rest volume, from the settings' rest positions; pressure needs both.
    if (e.pressureKPa > 0.0f) {
        ensureAnalysed(rec);
        if (e.closed && e.restVolumeM3 > 0.0f)
            mp.SetPressure(e.pressureKPa * 1000.0f * e.restVolumeM3);   // Jolt's n R T in joules: P x V0
        else {
            AVER_WARN("[Physics] soft body pressure ignored: the mesh is {} (needs a closed, outward wound shell)",
                      e.closed ? "inside out" : "not closed");
            e.pressureKPa = 0.0f;
        }
    }

    mp.SetEnableSkinConstraints(e.skinEnabled);
    mp.SetSkinnedMaxDistanceMultiplier(e.skinScale);
    if (e.group != 0) body.SetCollisionGroup(groupOf(e.group));

    // Attachments that came across a rebuild: the vertex is kinematic again in the new body, or the attachment drops.
    bool any = false;
    for (size_t i = 0; i < e.attachments.size();) {
        SoftAttachment& a = e.attachments[i];
        if (a.vertex < 0 || a.vertex >= rec.vertexCount) { e.attachments.erase(e.attachments.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        sbxSetVertexInvMass(rec, mp, a.vertex, 0.0f);
        zeroVelocity(mp, a.vertex);
        any = true;
        ++i;
    }
    if (any) {
        sbxRefreshRodInvMass(rec, mp);
        mp.CalculateMassAndInertia();
    }
}

void sbxEnsureAnalysed(SoftRecord& rec) {
    ensureAnalysed(rec);
}

void sbxBuilderDrop(int32_t b) {
    g_ext.erase(b);
}

void sbxDestroyAll() {
    g_ext.clear();
    g_sensorEvents.clear();
    g_groupFilter = nullptr;
}

void sbxSetVertexInvMass(SoftRecord& rec, JPH::SoftBodyMotionProperties& mp, int32_t vertex, float invMass) {
    mp.GetVertices()[static_cast<size_t>(vertex)].mInvMass = invMass;
    rec.settings->mVertices[static_cast<size_t>(vertex)].mInvMass = invMass;
}

void sbxRefreshRodInvMass(SoftRecord& rec, JPH::SoftBodyMotionProperties& mp) {
    const auto& verts = mp.GetVertices();
    for (JPH::SoftBodySharedSettings::RodStretchShear& r : rec.settings->mRodStretchShearConstraints)
        r.mInvMass = std::min(verts[r.mVertex[0]].mInvMass, verts[r.mVertex[1]].mInvMass);
}

// ---- the contact listener ----------------------------------------------------------------------------------

JPH::SoftBodyValidateResult sbxValidate(const SoftRecord& rec, int32_t otherHandle, const JPH::Body&,
                                        JPH::SoftBodyContactSettings& settings) {
    const SoftExt& e = rec.ext;
    if (otherHandle != 0 && !e.pairs.empty()) {
        const auto it = e.pairs.find(otherHandle);
        if (it != e.pairs.end()) {
            const SoftPairPolicy& p = it->second;
            if (p.mode == AVER_PHYS_SBX_PAIR_IGNORE) return JPH::SoftBodyValidateResult::RejectContact;
            if (p.mode == AVER_PHYS_SBX_PAIR_SENSOR) settings.mIsSensor = true;
            settings.mInvMassScale1 = p.soft;
            settings.mInvMassScale2 = p.other;
            settings.mInvInertiaScale2 = p.inertia;
            return JPH::SoftBodyValidateResult::AcceptContact;
        }
    }
    // Only a changed value is written, so a default policy leaves Jolt's own defaults untouched.
    if (e.softScale != 1.0f) settings.mInvMassScale1 = e.softScale;
    if (e.otherScale != 1.0f) settings.mInvMassScale2 = e.otherScale;
    if (e.inertiaScale != 1.0f) settings.mInvInertiaScale2 = e.inertiaScale;
    return JPH::SoftBodyValidateResult::AcceptContact;
}

// Worker thread, once per step per body that collided or touched a sensor. The set is diffed by sbxStepEnd.
void sbxContactAdded(SoftRecord& rec, const JPH::SoftBodyManifold& manifold) {
    const JPH::uint count = manifold.GetNumSensorContacts();
    if (count == 0 || !g_world) return;
    std::lock_guard<std::mutex> lock(eventMutex());
    for (JPH::uint i = 0; i < count; ++i) {
        const auto it = g_world->byId.find(manifold.GetSensorContactBodyID(i));
        if (it != g_world->byId.end()) rec.ext.sensorNow.insert(it->second);
    }
}

// ---- the fixed loop ----------------------------------------------------------------------------------------

bool sbxKeepAwake(const SoftRecord& rec) {
    return !rec.ext.attachments.empty() || rec.ext.submergedFraction > 0.0f;
}

void sbxBeginCall() {
    g_sensorEvents.clear();
}

namespace {

// Carries every attached vertex toward its target. The body is write-locked.
void stepAttachments(int32_t handle, SoftRecord& rec, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, float dt) {
    SoftExt& e = rec.ext;
    if (!(dt > 0.0f)) return;
    const JPH::RMat44 inv = body.GetCenterOfMassTransform().InversedRotationTranslation();
    auto& verts = mp.GetVertices();
    bool released = false;
    for (size_t i = 0; i < e.attachments.size();) {
        SoftAttachment& a = e.attachments[i];
        JPH::Vec3 targetWorld;
        if (a.otherBody != 0) {
            const auto oit = g_world->bodies.find(a.otherBody);
            if (oit == g_world->bodies.end()) {
                // The body this vertex followed is gone: let go, once, and say so.
                AVER_WARN("[Physics] soft body {}: the body vertex {} followed ({}) is gone; the vertex is released", handle, a.vertex, a.otherBody);
                sbxSetVertexInvMass(rec, mp, a.vertex, a.savedInvMass);
                zeroVelocity(mp, a.vertex);
                e.attachments.erase(e.attachments.begin() + static_cast<std::ptrdiff_t>(i));
                released = true;
                continue;
            }
            const JPH::RMat44 ow = g_world->system.GetBodyInterfaceNoLock().GetWorldTransform(oit->second);
            targetWorld = JPH::Vec3(ow * toJolt(a.targetCm));
        } else {
            targetWorld = toJolt(a.targetCm);
        }
        const JPH::Vec3 local = JPH::Vec3(inv * targetWorld);
        JPH::SoftBodyVertex& v = verts[static_cast<size_t>(a.vertex)];
        JPH::Vec3 vel = (local - v.mPosition) / dt;
        const float cap = cmToM(a.maxSpeedCmS);
        const float len = vel.Length();
        if (len > cap && len > 0.0f) vel *= cap / len;
        v.mVelocity = vel;
        ++i;
    }
    if (released) {
        sbxRefreshRodInvMass(rec, mp);
        mp.CalculateMassAndInertia();
    }
}

} // namespace

void sbxStepBegin(int32_t handle, SoftRecord& rec, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, float dt) {
    if (!rec.ext.attachments.empty()) stepAttachments(handle, rec, body, mp, dt);
    if (rec.ext.fluidDensity > 0.0f) sbxFluidStep(rec, body, mp, dt);
}

void sbxStepEnd(float) {
    if (!g_world) return;
    for (auto& [h, rec] : records()) {
        SoftExt& e = rec.ext;
        if (e.sensorNow.empty() && e.sensorPrev.empty()) continue;
        // A sleeping or deactivated body ran no update: its inside set stands, and an empty "now" would read as an exit.
        const auto bit = g_world->bodies.find(h);
        if (rec.frozen || bit == g_world->bodies.end() || !g_world->system.GetBodyInterface().IsActive(bit->second)) {
            e.sensorNow.clear();
            continue;
        }
        for (const int32_t s : e.sensorNow)
            if (!e.sensorPrev.count(s)) g_sensorEvents.push_back({h, s, 1});
        for (const int32_t s : e.sensorPrev)
            if (!e.sensorNow.count(s)) g_sensorEvents.push_back({h, s, 0});
        e.sensorPrev.swap(e.sensorNow);
        e.sensorNow.clear();
    }
}

// LARGE_WORLD: the soft bodies themselves move with their rigid body, but the world-space numbers the sbx calls keep do not: a world attachment target and an own buoyancy
// plane point. A floating-origin shift moves them after the bodies (PhysicsOrigin.hpp), so a vertex pinned to a world point is not dragged 16 km back.
namespace {
void shiftSoftWorldState(const aver::physics::detail::Vec3d& d) {
    auto sub = [&d](aver::Vec3& p) {
        p.x = static_cast<float>(static_cast<double>(p.x) - d.x);
        p.y = static_cast<float>(static_cast<double>(p.y) - d.y);
        p.z = static_cast<float>(static_cast<double>(p.z) - d.z);
    };
    for (auto& entry : records()) {
        SoftExt& e = entry.second.ext;
        for (SoftAttachment& a : e.attachments)
            if (a.otherBody == 0) sub(a.targetCm);   // a fixed world target; a body-relative one is an offset in that body's frame
        if (e.hasOwnPlane) sub(e.planePointCm);
    }
}
const struct OriginHookRegistrar {
    OriginHookRegistrar() { aver::physics::detail::physicsOriginHook("softbody.world_state", &shiftSoftWorldState); }
} g_originHookRegistrar;
} // namespace
} // namespace aver::physics::softbody

// ---- the ABI -----------------------------------------------------------------------------------------------

extern "C" {

int32_t aver_phys_sbx_abi_version(void) { return AVER_PHYS_SBX_ABI_VERSION; }

// ---- builder ----

int32_t aver_phys_sbx_builder_volumes(int32_t b, const int32_t* tet4, const float* compliance, int32_t count) {
    if (count < 0) return refuse(AbiError::InvalidArgument);
    if (count > 0 && !tet4) return refuse(AbiError::NullPointer);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    const int32_t n = builderVertexCount(b);
    for (int32_t t = 0; t < count; ++t) {
        const int32_t* q = tet4 + t * 4;
        for (int x = 0; x < 4; ++x) {
            if (q[x] < 0 || q[x] >= n) return refuse(AbiError::OutOfRange);
            for (int y = x + 1; y < 4; ++y)
                if (q[x] == q[y]) return refuse(AbiError::InvalidArgument);
        }
        if (compliance && !finiteNonNeg(compliance[t])) return refuse(AbiError::InvalidArgument);
    }
    for (int32_t t = 0; t < count; ++t) {
        E->tets.insert(E->tets.end(), tet4 + t * 4, tet4 + t * 4 + 4);
        E->tetCompliance.push_back(compliance ? compliance[t] : 0.0f);
    }
    return succeed();
}

int32_t aver_phys_sbx_builder_lra(int32_t b, const int32_t* pairs2, const float* maxDistCm, int32_t count) {
    if (count < 0) return refuse(AbiError::InvalidArgument);
    if (count > 0 && (!pairs2 || !maxDistCm)) return refuse(AbiError::NullPointer);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    const int32_t n = builderVertexCount(b);
    for (int32_t i = 0; i < count; ++i) {
        const int32_t a = pairs2[i * 2], c = pairs2[i * 2 + 1];
        if (a < 0 || c < 0 || a >= n || c >= n) return refuse(AbiError::OutOfRange);
        if (a == c || !std::isfinite(maxDistCm[i]) || !(maxDistCm[i] > 0.0f)) return refuse(AbiError::InvalidArgument);
    }
    E->lra.insert(E->lra.end(), pairs2, pairs2 + static_cast<size_t>(count) * 2);
    E->lraMaxCm.insert(E->lraMaxCm.end(), maxDistCm, maxDistCm + count);
    return succeed();
}

int32_t aver_phys_sbx_builder_lra_auto(int32_t b, int32_t mode, float distanceMultiplier) {
    if (mode < 0 || mode > 2 || !std::isfinite(distanceMultiplier) || !(distanceMultiplier > 0.0f)) return refuse(AbiError::InvalidArgument);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    E->lraAutoMode = mode;
    E->lraAutoMult = distanceMultiplier;
    return succeed();
}

int32_t aver_phys_sbx_builder_rods(int32_t b, const int32_t* ab, const float* stretchCompliance, int32_t count,
                                   const int32_t* rodPairs2, const float* bendTwistCompliance, int32_t pairCount) {
    if (count < 0 || pairCount < 0) return refuse(AbiError::InvalidArgument);
    if ((count > 0 && !ab) || (pairCount > 0 && !rodPairs2)) return refuse(AbiError::NullPointer);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    const int32_t n = builderVertexCount(b);
    for (int32_t r = 0; r < count; ++r) {
        const int32_t a = ab[r * 2], c = ab[r * 2 + 1];
        if (a < 0 || c < 0 || a >= n || c >= n) return refuse(AbiError::OutOfRange);
        if (a == c) return refuse(AbiError::InvalidArgument);
        if (stretchCompliance && !finiteNonNeg(stretchCompliance[r])) return refuse(AbiError::InvalidArgument);
    }
    std::vector<char> covered(static_cast<size_t>(count), 0);
    for (int32_t p = 0; p < pairCount; ++p) {
        const int32_t r0 = rodPairs2[p * 2], r1 = rodPairs2[p * 2 + 1];
        if (r0 < 0 || r1 < 0 || r0 >= count || r1 >= count) return refuse(AbiError::OutOfRange);
        if (r0 == r1) return refuse(AbiError::InvalidArgument);
        if (bendTwistCompliance && !finiteNonNeg(bendTwistCompliance[p])) return refuse(AbiError::InvalidArgument);
        covered[static_cast<size_t>(r0)] = covered[static_cast<size_t>(r1)] = 1;
    }
    // A rod no bend-twist constraint holds spins about its axis at constant speed (Jolt's own warning).
    for (const char c : covered)
        if (!c) return refuse(AbiError::InvalidArgument);
    const int32_t base = static_cast<int32_t>(E->rodAb.size() / 2);
    E->rodAb.insert(E->rodAb.end(), ab, ab + static_cast<size_t>(count) * 2);
    for (int32_t r = 0; r < count; ++r) E->rodStretch.push_back(stretchCompliance ? stretchCompliance[r] : 0.0f);
    for (int32_t p = 0; p < pairCount; ++p) {
        E->pairAb.push_back(base + rodPairs2[p * 2]);
        E->pairAb.push_back(base + rodPairs2[p * 2 + 1]);
        E->pairCompliance.push_back(bendTwistCompliance ? bendTwistCompliance[p] : 0.0f);
    }
    return succeed();
}

int32_t aver_phys_sbx_builder_rod_chain(int32_t b, int32_t firstVertex, int32_t vertexCount, float stretchCompliance,
                                        float bendTwistCompliance) {
    if (vertexCount < 3 || firstVertex < 0 || !finiteNonNeg(stretchCompliance) || !finiteNonNeg(bendTwistCompliance))
        return refuse(AbiError::InvalidArgument);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    if (firstVertex + vertexCount > builderVertexCount(b)) return refuse(AbiError::OutOfRange);
    const int32_t rods = vertexCount - 1, base = static_cast<int32_t>(E->rodAb.size() / 2);
    for (int32_t r = 0; r < rods; ++r) {
        E->rodAb.push_back(firstVertex + r);
        E->rodAb.push_back(firstVertex + r + 1);
        E->rodStretch.push_back(stretchCompliance);
    }
    for (int32_t r = 0; r + 1 < rods; ++r) {
        E->pairAb.push_back(base + r);
        E->pairAb.push_back(base + r + 1);
        E->pairCompliance.push_back(bendTwistCompliance);
    }
    return succeed();
}

int32_t aver_phys_sbx_builder_auto(int32_t b, const float* attrs5, int32_t attrCount, int32_t bendType, float angleToleranceDeg) {
    if (!attrs5) return refuse(AbiError::NullPointer);
    if (attrCount < 1 || bendType < 0 || bendType > 2 || !std::isfinite(angleToleranceDeg) || !(angleToleranceDeg > 0.0f) ||
        angleToleranceDeg > 45.0f)
        return refuse(AbiError::InvalidArgument);
    for (int32_t i = 0; i < attrCount * 5; ++i)
        if (!std::isfinite(attrs5[i])) return refuse(AbiError::InvalidArgument);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    E->autoStaged = true;
    E->attrs.assign(attrs5, attrs5 + static_cast<size_t>(attrCount) * 5);
    E->autoBend = bendType;
    E->autoAngleDeg = angleToleranceDeg;
    return succeed();
}

int32_t aver_phys_sbx_builder_params(int32_t b, const float* p, int32_t n) {
    if (!p || n < 0) return refuse(AbiError::NullPointer);
    BuilderExt* E = stage(b);
    if (!E) return 0;
    for (int32_t i = 0; i < n && i < kP; ++i)
        if (!std::isnan(p[i])) E->params[i] = p[i];
    return succeed();
}

int32_t aver_phys_sbx_slots(int32_t body, int32_t kind, int32_t* outSlots, int32_t maxCount) {
    const SoftRecord* r = recordOf(body);
    if (!r) return refuse(AbiError::BadHandle);
    const std::vector<int32_t>* v = nullptr;
    switch (kind) {
        case 0: v = &r->ext.volumeSlot; break;
        case 1: v = &r->ext.lraSlot; break;
        case 2: v = &r->ext.rodSlot; break;
        case 3: v = &r->ext.rodPairSlot; break;
        default: return refuse(AbiError::InvalidArgument);
    }
    if (outSlots)
        for (int32_t i = 0; i < maxCount && i < static_cast<int32_t>(v->size()); ++i) outSlots[i] = (*v)[static_cast<size_t>(i)];
    setAbiError(AbiError::Ok);
    return static_cast<int32_t>(v->size());
}

// ---- live ----

int32_t aver_phys_sbx_set_pressure(int32_t body, float pressureKPa) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!std::isfinite(pressureKPa) || pressureKPa < 0.0f) return refuse(AbiError::InvalidArgument);
    ensureAnalysed(*r);
    if (pressureKPa > 0.0f && (!r->ext.closed || !(r->ext.restVolumeM3 > 0.0f))) return refuse(AbiError::Unsupported);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    const bool changed = r->ext.pressureKPa != pressureKPa;
    mp->SetPressure(pressureKPa * 1000.0f * std::max(0.0f, r->ext.restVolumeM3));
    r->ext.pressureKPa = pressureKPa;
    if (changed) wake(*r, *id);
    return succeed();
}

int32_t aver_phys_sbx_get_pressure(int32_t body, float* outKPa) {
    const JPH::BodyID* id = nullptr;
    const SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!outKPa) return refuse(AbiError::NullPointer);
    *outKPa = r->ext.pressureKPa;
    return succeed();
}

int32_t aver_phys_sbx_volume(int32_t body, float* outRestCm3, float* outNowCm3) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    ensureAnalysed(*r);
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    const auto* mp = static_cast<const JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    constexpr float kCm3PerM3 = kCmPerMetre * kCmPerMetre * kCmPerMetre;
    if (outRestCm3) *outRestCm3 = std::fabs(r->ext.restVolumeM3) * kCm3PerM3;
    if (outNowCm3) *outNowCm3 = mp->GetVolume() * kCm3PerM3;
    return succeed();
}

int32_t aver_phys_sbx_set_skin(int32_t body, int32_t enabled, float distanceScale) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!finiteNonNeg(distanceScale)) return refuse(AbiError::InvalidArgument);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    r->ext.skinEnabled = enabled != 0;
    r->ext.skinScale = distanceScale;
    mp->SetEnableSkinConstraints(r->ext.skinEnabled);
    mp->SetSkinnedMaxDistanceMultiplier(distanceScale);
    wake(*r, *id);
    return succeed();
}

int32_t aver_phys_sbx_set_inv_mass(int32_t body, const int32_t* vertex, const float* invMass, int32_t n) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (n < 0 || (n > 0 && (!vertex || !invMass))) return refuse(AbiError::NullPointer);
    for (int32_t i = 0; i < n; ++i) {
        if (vertex[i] < 0 || vertex[i] >= r->vertexCount) return refuse(AbiError::OutOfRange);
        if (!finiteNonNeg(invMass[i])) return refuse(AbiError::InvalidArgument);
    }
    if (n == 0) return succeed();
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    for (int32_t i = 0; i < n; ++i) {
        // An attached vertex stays pinned: the value is what it gets back on detach.
        if (SoftAttachment* a = attachmentOf(r->ext, vertex[i])) { a->savedInvMass = invMass[i]; continue; }
        sbxSetVertexInvMass(*r, *mp, vertex[i], invMass[i]);
        // A kinematic vertex is carried by its velocity for good, so a pin must not keep the speed it had.
        if (invMass[i] == 0.0f) zeroVelocity(*mp, vertex[i]);
    }
    sbxRefreshRodInvMass(*r, *mp);
    mp->CalculateMassAndInertia();
    wake(*r, *id);
    return succeed();
}

// ---- attachments ----

int32_t aver_phys_sbx_attach(int32_t body, int32_t vertex, const float* targetXyzCm, float maxSpeedCmS) {
    return attachImpl(body, vertex, 0, targetXyzCm, maxSpeedCmS);
}

int32_t aver_phys_sbx_attach_to_body(int32_t body, int32_t vertex, int32_t otherBody, const float* localOffsetXyzCm, float maxSpeedCmS) {
    if (otherBody == 0) return refuse(AbiError::BadHandle);
    return attachImpl(body, vertex, otherBody, localOffsetXyzCm, maxSpeedCmS);
}

int32_t aver_phys_sbx_move_attachment(int32_t body, int32_t vertex, const float* targetXyzCm) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!targetXyzCm) return refuse(AbiError::NullPointer);
    if (!std::isfinite(targetXyzCm[0]) || !std::isfinite(targetXyzCm[1]) || !std::isfinite(targetXyzCm[2])) return refuse(AbiError::InvalidArgument);
    SoftAttachment* a = attachmentOf(r->ext, vertex);
    if (!a || a->otherBody != 0) return refuse(AbiError::InvalidArgument);
    a->targetCm = aver::Vec3(targetXyzCm[0], targetXyzCm[1], targetXyzCm[2]);
    r->frozen = false;
    g_world->system.GetBodyInterface().ActivateBody(*id);
    return succeed();
}

int32_t aver_phys_sbx_detach(int32_t body, int32_t vertex, float restoreInvMass) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!std::isfinite(restoreInvMass)) return refuse(AbiError::InvalidArgument);
    SoftAttachment* a = attachmentOf(r->ext, vertex);
    if (!a) return refuse(AbiError::InvalidArgument);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    const float restore = restoreInvMass < 0.0f ? a->savedInvMass : restoreInvMass;
    sbxSetVertexInvMass(*r, *mp, vertex, restore);
    zeroVelocity(*mp, vertex);
    r->ext.attachments.erase(r->ext.attachments.begin() + (a - r->ext.attachments.data()));
    sbxRefreshRodInvMass(*r, *mp);
    mp->CalculateMassAndInertia();
    wake(*r, *id);
    return succeed();
}

int32_t aver_phys_sbx_attachment_count(int32_t body) {
    const SoftRecord* r = recordOf(body);
    return r ? static_cast<int32_t>(r->ext.attachments.size()) : 0;
}

// ---- contact policy, sensors, group ----

int32_t aver_phys_sbx_set_contact_policy(int32_t body, float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!finiteNonNeg(softInvMassScale) || !finiteNonNeg(otherInvMassScale) || !finiteNonNeg(otherInvInertiaScale))
        return refuse(AbiError::InvalidArgument);
    r->ext.softScale = softInvMassScale;
    r->ext.otherScale = otherInvMassScale;
    r->ext.inertiaScale = otherInvInertiaScale;
    return succeed();
}

int32_t aver_phys_sbx_set_pair_policy(int32_t body, int32_t otherBody, int32_t mode, float softInvMassScale,
                                      float otherInvMassScale, float otherInvInertiaScale) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (mode < AVER_PHYS_SBX_PAIR_NORMAL || mode > AVER_PHYS_SBX_PAIR_IGNORE || !finiteNonNeg(softInvMassScale) ||
        !finiteNonNeg(otherInvMassScale) || !finiteNonNeg(otherInvInertiaScale))
        return refuse(AbiError::InvalidArgument);
    if (otherBody == body || !findBody(otherBody)) return otherBody == body ? refuse(AbiError::InvalidArgument) : 0;
    SoftPairPolicy p;
    p.mode = mode;
    p.soft = softInvMassScale;
    p.other = otherInvMassScale;
    p.inertia = otherInvInertiaScale;
    r->ext.pairs[otherBody] = p;
    return succeed();
}

int32_t aver_phys_sbx_clear_pair_policy(int32_t body, int32_t otherBody) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    r->ext.pairs.erase(otherBody);
    return succeed();
}

int32_t aver_phys_sbx_sensor_count(void) { return static_cast<int32_t>(g_sensorEvents.size()); }

int32_t aver_phys_sbx_sensor_get(int32_t index, int32_t* outSoftBody, int32_t* outSensorBody, int32_t* outEntered) {
    if (index < 0 || index >= static_cast<int32_t>(g_sensorEvents.size())) return refuse(AbiError::OutOfRange);
    const SensorEvent& e = g_sensorEvents[static_cast<size_t>(index)];
    if (outSoftBody) *outSoftBody = e.soft;
    if (outSensorBody) *outSensorBody = e.sensor;
    if (outEntered) *outEntered = e.entered;
    return succeed();
}

int32_t aver_phys_sbx_set_collision_group(int32_t body, int32_t group) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (group < 0) return refuse(AbiError::InvalidArgument);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    lock.GetBody().SetCollisionGroup(groupOf(group));
    r->ext.group = group;
    return succeed();
}

// ---- buoyancy ----

int32_t aver_phys_sbx_set_buoyancy(int32_t body, float fluidDensityKgM3, float drag, int32_t flags) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!finiteNonNeg(fluidDensityKgM3) || !finiteNonNeg(drag)) return refuse(AbiError::InvalidArgument);
    r->ext.fluidDensity = fluidDensityKgM3;
    r->ext.fluidDrag = drag;
    r->ext.buoyFlags = flags;
    if (fluidDensityKgM3 <= 0.0f) { r->ext.submergedFraction = 0.0f; r->ext.submergedVolumeCm3 = 0.0f; }
    return succeed();
}

int32_t aver_phys_sbx_set_water_plane(int32_t body, const float* pointXyzCm, const float* normalXyz, const float* currentXyzCmS) {
    const JPH::BodyID* id = nullptr;
    SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    if (!pointXyzCm || !normalXyz) return refuse(AbiError::NullPointer);
    const aver::Vec3 n(normalXyz[0], normalXyz[1], normalXyz[2]);
    if (!std::isfinite(pointXyzCm[0]) || !std::isfinite(pointXyzCm[1]) || !std::isfinite(pointXyzCm[2]) || !std::isfinite(n.x) ||
        !std::isfinite(n.y) || !std::isfinite(n.z) || n.sizeSquared() < 1.0e-12f)
        return refuse(AbiError::InvalidArgument);
    aver::Vec3 cur(0, 0, 0);
    if (currentXyzCmS) {
        cur = aver::Vec3(currentXyzCmS[0], currentXyzCmS[1], currentXyzCmS[2]);
        if (!std::isfinite(cur.x) || !std::isfinite(cur.y) || !std::isfinite(cur.z)) return refuse(AbiError::InvalidArgument);
    }
    r->ext.hasOwnPlane = true;
    r->ext.planePointCm = aver::Vec3(pointXyzCm[0], pointXyzCm[1], pointXyzCm[2]);
    r->ext.planeNormal = n.getSafeNormal();
    r->ext.planeCurrentCmS = cur;
    return succeed();
}

int32_t aver_phys_sbx_submerged(int32_t body, float* outFraction01, float* outVolumeCm3) {
    const SoftRecord* r = recordOf(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (outFraction01) *outFraction01 = r->ext.submergedFraction;
    if (outVolumeCm3) *outVolumeCm3 = r->ext.submergedVolumeCm3;
    return succeed();
}

// ---- rods ----

int32_t aver_phys_sbx_rod_count(int32_t body) {
    const SoftRecord* r = recordOf(body);
    return r ? static_cast<int32_t>(r->settings->mRodStretchShearConstraints.size()) : 0;
}

int32_t aver_phys_sbx_rod_state(int32_t body, int32_t firstRod, int32_t count, float* outQuatXyzw, float* outAngVelRadS) {
    const JPH::BodyID* id = nullptr;
    const SoftRecord* r = liveRecord(body, id);
    if (!r) return 0;
    const int32_t rods = static_cast<int32_t>(r->settings->mRodStretchShearConstraints.size());
    if (firstRod < 0 || count < 0 || firstRod + count > rods) return refuse(AbiError::OutOfRange);
    if (count == 0) return 0;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    const JPH::Body& b = lock.GetBody();
    const auto* mp = static_cast<const JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
    const JPH::RMat44 com = b.GetCenterOfMassTransform();
    const JPH::Quat comRot = com.GetQuaternion();
    // The rod's axis is local +Z in Jolt. A half turn about Y first makes the engine +X (which is Jolt -Z) land on it,
    // so "engine +X rotated by the result" is the rod's direction.
    const JPH::Quat flip(0.0f, 1.0f, 0.0f, 0.0f);
    for (int32_t i = 0; i < count; ++i) {
        const JPH::uint rod = static_cast<JPH::uint>(firstRod + i);
        if (outQuatXyzw) {
            const aver::Quat q = fromJolt(comRot * mp->GetRodRotation(rod) * flip);
            outQuatXyzw[i * 4 + 0] = q.x; outQuatXyzw[i * 4 + 1] = q.y; outQuatXyzw[i * 4 + 2] = q.z; outQuatXyzw[i * 4 + 3] = q.w;
        }
        if (outAngVelRadS) {
            const aver::Vec3 w = fromJoltAngular(comRot * mp->GetRodAngularVelocity(rod));
            outAngVelRadS[i * 3 + 0] = w.x; outAngVelRadS[i * 3 + 1] = w.y; outAngVelRadS[i * 3 + 2] = w.z;
        }
    }
    return count;
}

} // extern "C"
