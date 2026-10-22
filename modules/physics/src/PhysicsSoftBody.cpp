// PhysicsSoftBody.cpp -- builder-style soft bodies that can dent, tear and be rebuilt (physics_softbody_abi.h).
//
// ITS OWN TRANSLATION UNIT, like the joints and the vehicles: a flat sheet of entry points over the handle
// tables in PhysicsInternal.hpp, with the registry (one record per body this file built) and the soft-body
// contact listener local to it. The older aver_phys_softbody_* functions stay in PhysicsWorld.cpp untouched.
//
// WHAT THE REGISTRY BUYS. Jolt reads an edge's mRestLength and a dihedral bend's mInitialAngle from the
// SHARED SETTINGS on every iteration, so a dent is an in-place write on settings this file owns. It owns a
// non-const Ref per body (never shared between bodies) precisely so that write is legal. Removing edges or
// duplicating vertices changes arrays Jolt sized at creation, so a tear is a rebuild: finish() with
// replaceBody keeps the handle and carries the vertex state across.
//
// The record type itself lives in SoftBodyInternal.hpp now: the wave 11 calls (physics_softbody_ext_abi.h,
// PhysicsSoftBodyExt.cpp and SoftBodyFluid.cpp) keep their state in its SoftExt member, and finish() and the
// listener call their sbx* hooks. A body that never calls those behaves exactly as before.
#include "aver/physics/physics_softbody_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "SoftBodyAero.hpp"
#include "SoftBodyInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Trigonometry.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;
using aver::physics::softbody::SoftRecord;

namespace {

constexpr int32_t kMaxVertices = 65536;
constexpr int kParamCount = AVER_PHYS_SB_P_COUNT;

float nanf32() { return std::nanf(""); }

void defaultParams(float* p) { softbody::defaultSoftParams(p); }

void overlayParams(float* dst, const float* src, int32_t n) {
    for (int32_t i = 0; i < n && i < kParamCount; ++i)
        if (!std::isnan(src[i])) dst[i] = src[i];
}

struct Builder {
    int32_t vertexCount = 0;
    bool hasVertices = false;
    std::vector<float> rest, invMass, statePos, stateVel;
    std::vector<int32_t> faces;                       // 3 per triangle
    std::vector<int32_t> edgeAb;                      // 2 per edge
    std::vector<float> edgeRest, edgeCompliance;
    std::vector<int32_t> bendQuad;                    // 4 per bend
    std::vector<float> bendCompliance;
    bool hasSkin = false;
    std::vector<int32_t> joints4;
    std::vector<float> weights4, reach;
    float backstopCm = -1.0f;
    int32_t jointCount = 0;
    float params[kParamCount];
    Builder() { for (float& p : params) p = nanf32(); }
};

std::unordered_map<int32_t, Builder> g_builders;
int32_t g_nextBuilder = 1;
std::map<int32_t, SoftRecord> g_records;              // ordered: the aero loop and events follow handle order
softbody::WindParams g_wind;
uint64_t g_step = 0;

struct ImpactEvent {
    int32_t soft = 0, other = 0;
    float point[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
    float closingCmS = 0.0f, otherMassKg = 0.0f;
    int32_t vertices = 0;
};
std::mutex g_eventMutex;
std::vector<ImpactEvent> g_events;
struct PairSlot { JPH::Vec3 vel = JPH::Vec3::sZero(); float invMass = 0.0f; };
std::map<uint64_t, PairSlot> g_pairs;
uint64_t pairKey(int32_t soft, int32_t other) { return (static_cast<uint64_t>(static_cast<uint32_t>(soft)) << 32) | static_cast<uint32_t>(other); }

int32_t refuse(AbiError e) {
    setAbiError(e);
    return 0;
}

SoftRecord* findRecord(int32_t h) {
    const auto it = g_records.find(h);
    return it == g_records.end() ? nullptr : &it->second;
}

int clampLayer(float v) { return std::clamp(static_cast<int>(std::lround(v)), 0, static_cast<int>(Layers::kUserLayerCount) - 1); }

// Creation settings from a params array.
void paramsToCreation(JPH::SoftBodyCreationSettings& s, const float* p) {
    s.mNumIterations = static_cast<JPH::uint32>(std::clamp(static_cast<int>(std::lround(p[AVER_PHYS_SB_P_ITERATIONS])), 1, 64));
    s.mLinearDamping = std::max(0.0f, p[AVER_PHYS_SB_P_DAMPING]);
    s.mFriction = std::max(0.0f, p[AVER_PHYS_SB_P_FRICTION]);
    s.mRestitution = std::clamp(p[AVER_PHYS_SB_P_RESTITUTION], 0.0f, 1.0f);
    s.mGravityFactor = p[AVER_PHYS_SB_P_GRAVITY_SCALE];
    s.mVertexRadius = cmToM(std::max(0.0f, p[AVER_PHYS_SB_P_VERTEX_RADIUS_CM]));
    s.mMaxLinearVelocity = cmToM(std::max(1.0f, p[AVER_PHYS_SB_P_MAX_SPEED_CMS]));
    s.mAllowSleeping = p[AVER_PHYS_SB_P_ALLOW_SLEEP] >= 0.5f;
    s.mFacesDoubleSided = p[AVER_PHYS_SB_P_DOUBLE_SIDED] >= 0.5f;
    s.mObjectLayer = Layers::encode(static_cast<u32>(clampLayer(p[AVER_PHYS_SB_P_LAYER])), true);
    s.mUpdatePosition = false;   // the body origin stays put; the particles move (as addSoftBody)
}

// Composes the palette with the entity world and the body's COM and gives it to Jolt.
// M' = T(com) * skin[j] * entityWorld * T(-com) in the engine's row-vector order.
void pushPalette(JPH::Body& body, JPH::SoftBodyMotionProperties& mp, const float* joints, int32_t n,
                 const float* entityWorld16, bool hard) {
    Mat4 ew;
    if (entityWorld16)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) ew.m[r][c] = entityWorld16[r * 4 + c];
    const JPH::RMat44 com = body.GetCenterOfMassTransform();
    const Vec3 comCm = fromJolt(JPH::Vec3(com.GetTranslation()));
    const Mat4 tIn = Mat4::translation(comCm), tOut = Mat4::translation(-comCm);
    std::vector<JPH::Mat44> palette(static_cast<size_t>(n));
    for (int32_t j = 0; j < n; ++j) {
        Mat4 m;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m.m[r][c] = joints[j * 16 + r * 4 + c];
        palette[static_cast<size_t>(j)] = toJolt(tIn * m * ew * tOut);
    }
    mp.SkinVertices(com, palette.data(), static_cast<JPH::uint>(n), hard, *g_world->temp);
}

// ---- the soft-body contact listener (impacts) ----------------------------------------------------------

class ImpactListener final : public JPH::SoftBodyContactListener {
public:
    JPH::SoftBodyValidateResult OnSoftBodyContactValidate(const JPH::Body& soft, const JPH::Body& other,
                                                          JPH::SoftBodyContactSettings& settings) override {
        if (!g_world) return JPH::SoftBodyValidateResult::AcceptContact;
        const auto sit = g_world->byId.find(soft.GetID());
        if (sit == g_world->byId.end()) return JPH::SoftBodyValidateResult::AcceptContact;
        const int32_t hs = sit->second;
        const auto rit = g_records.find(hs);
        if (rit == g_records.end()) return JPH::SoftBodyValidateResult::AcceptContact;
        const auto oit = g_world->byId.find(other.GetID());
        // The contact policy of the wave 11 ABI (mass scales, per-pair ignore and sensor); a default policy changes nothing.
        if (softbody::sbxValidate(rit->second, oit == g_world->byId.end() ? 0 : oit->second, other, settings) ==
            JPH::SoftBodyValidateResult::RejectContact)
            return JPH::SoftBodyValidateResult::RejectContact;
        if (!rit->second.impacts) return JPH::SoftBodyValidateResult::AcceptContact;
        if (oit == g_world->byId.end()) return JPH::SoftBodyValidateResult::AcceptContact;
        PairSlot slot;
        // Read-only: the other body's velocity and inverse mass as the soft-body phase sees them.
        if (!other.IsStatic()) {
            slot.vel = other.GetLinearVelocity();
            slot.invMass = other.IsDynamic() ? other.GetMotionProperties()->GetInverseMass() : 0.0f;
        }
        std::lock_guard<std::mutex> lock(g_eventMutex);
        g_pairs[pairKey(hs, oit->second)] = slot;
        return JPH::SoftBodyValidateResult::AcceptContact;
    }

    void OnSoftBodyContactAdded(const JPH::Body& soft, const JPH::SoftBodyManifold& manifold) override {
        if (!g_world) return;
        const auto sit = g_world->byId.find(soft.GetID());
        if (sit == g_world->byId.end()) return;
        const int32_t hs = sit->second;
        const auto rit = g_records.find(hs);
        if (rit == g_records.end()) return;
        SoftRecord& rec = rit->second;
        softbody::sbxContactAdded(rec, manifold);   // the sensor set (takes the event mutex itself)
        if (!rec.impacts) return;
        const auto& verts = manifold.GetVertices();
        if (rec.velBefore.size() != verts.size()) return;
        const JPH::RMat44 com = soft.GetCenterOfMassTransform();

        struct Acc { int32_t other; JPH::Vec3 point, normal; float closing; int32_t count; float invMass; };
        std::vector<Acc> acc;
        std::lock_guard<std::mutex> lock(g_eventMutex);
        for (size_t i = 0; i < verts.size(); ++i) {
            const JPH::SoftBodyVertex& v = verts[i];
            if (!manifold.HasContact(v)) continue;
            const JPH::BodyID oid = manifold.GetContactBodyID(v);
            if (oid.IsInvalid()) continue;
            const auto oit = g_world->byId.find(oid);
            if (oit == g_world->byId.end()) continue;
            const int32_t ho = oit->second;
            PairSlot slot;
            const auto pit = g_pairs.find(pairKey(hs, ho));
            if (pit != g_pairs.end()) slot = pit->second;
            // The plane normal points out of the other body toward the vertex.
            const JPH::Vec3 nOut = com.Multiply3x3(-manifold.GetContactNormal(v));
            const JPH::Vec3 vBefore = com.Multiply3x3(rec.velBefore[i]);
            const float closing = std::max(0.0f, (slot.vel - vBefore).Dot(nOut));
            const JPH::Vec3 point = JPH::Vec3(com * manifold.GetLocalContactPoint(v));
            Acc* a = nullptr;
            for (Acc& e : acc) if (e.other == ho) { a = &e; break; }
            if (!a) { acc.push_back({ho, JPH::Vec3::sZero(), JPH::Vec3::sZero(), 0.0f, 0, slot.invMass}); a = &acc.back(); }
            a->point += point;
            a->normal += nOut;
            a->closing = std::max(a->closing, closing);
            ++a->count;
        }
        for (const Acc& a : acc) {
            const float closingCm = a.closing * kCmPerMetre;
            if (closingCm < 1.0f) continue;
            ImpactEvent e;
            e.soft = hs; e.other = a.other;
            const Vec3 p = fromJolt(a.point / static_cast<float>(a.count));
            const Vec3 n = fromJoltUnit(a.normal.NormalizedOr(JPH::Vec3::sAxisY()));
            e.point[0] = p.x; e.point[1] = p.y; e.point[2] = p.z;
            e.normal[0] = n.x; e.normal[1] = n.y; e.normal[2] = n.z;
            e.closingCmS = closingCm;
            e.otherMassKg = a.invMass > 0.0f ? 1.0f / a.invMass : 0.0f;
            e.vertices = a.count;
            g_events.push_back(e);
        }
    }
};

ImpactListener g_listener;

} // namespace

// ---- the PhysicsWorld.cpp hooks ----------------------------------------------------------------------------

namespace aver::physics::softbody {

void installListener() {
    if (g_world) g_world->system.SetSoftBodyContactListener(&g_listener);
}

void removeListener() {
    if (g_world) g_world->system.SetSoftBodyContactListener(nullptr);
}

// The registry, for the wave 11 translation units (SoftBodyInternal.hpp).
std::map<int32_t, SoftRecord>& records() { return g_records; }
SoftRecord* recordOf(int32_t body) { return findRecord(body); }
std::mutex& eventMutex() { return g_eventMutex; }
int32_t builderVertexCount(int32_t b) {
    const auto it = g_builders.find(b);
    return it == g_builders.end() ? 0 : it->second.vertexCount;
}

void beginCall() {
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        g_events.clear();
        g_pairs.clear();
    }
    sbxBeginCall();
}

void onStepBegin(float dt) {
    if (g_world && !g_records.empty()) {
        const bool windOn = g_wind.any();
        for (auto& [h, rec] : g_records) {
            if (rec.frozen) continue;
            const auto bit = g_world->bodies.find(h);
            if (bit == g_world->bodies.end()) continue;
            JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), bit->second);
            if (!lock.Succeeded()) continue;
            JPH::Body& body = lock.GetBody();
            if (!body.IsSoftBody()) continue;
            auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());

            if (rec.impacts) {
                const auto& verts = mp->GetVertices();
                rec.velBefore.resize(verts.size());
                for (size_t i = 0; i < verts.size(); ++i) rec.velBefore[i] = verts[i].mVelocity;
            }
            const bool aero = rec.drag > 0.0f;
            const bool aeroOn = aero && windOn;
            // Attachments and a submerged face (the wave 11 calls) keep a body awake the way aero does.
            const bool keep = aeroOn || (aero && (rec.aeroFlags & 1) != 0) || sbxKeepAwake(rec);
            if (keep && !rec.sleepForcedOff) { body.SetAllowSleeping(false); rec.sleepForcedOff = true; }
            else if (!keep && rec.sleepForcedOff) { body.SetAllowSleeping(rec.params[AVER_PHYS_SB_P_ALLOW_SLEEP] >= 0.5f); rec.sleepForcedOff = false; }
            if (keep && !body.IsActive()) g_world->system.GetBodyInterfaceNoLock().ActivateBody(body.GetID());
            if (aeroOn) applyAero(*mp, body.GetCenterOfMassTransform(), rec.drag, rec.lift, g_wind, g_step, dt);
            sbxStepBegin(h, rec, body, *mp, dt);   // attachments, then buoyancy
        }
    }
    ++g_step;
}

// After system.Update, beside vehicleStepEnd: sensor enter and exit events.
void onStepEnd(float dt) {
    sbxStepEnd(dt);
}

void onBodyRemoved(int32_t handle) {
    g_records.erase(handle);
}

void destroyAll() {
    g_records.clear();
    g_builders.clear();
    g_nextBuilder = 1;
    g_wind = WindParams{};
    g_step = 0;
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        g_events.clear();
        g_pairs.clear();
    }
    sbxDestroyAll();
}

} // namespace aver::physics::softbody

// ---- the ABI -----------------------------------------------------------------------------------------------

extern "C" {

int32_t aver_phys_sb_builder_begin(int32_t vertexCount) {
    if (!g_world) return refuse(AbiError::NotInitialised);
    if (vertexCount < 1 || vertexCount > kMaxVertices) return refuse(AbiError::InvalidArgument);
    const int32_t id = g_nextBuilder++;
    g_builders[id].vertexCount = vertexCount;
    return id;
}

int32_t aver_phys_sb_builder_vertices(int32_t b, const float* restXyz, const float* invMass,
                                      const float* statePosXyz, const float* stateVelXyz) {
    const auto it = g_builders.find(b);
    if (it == g_builders.end()) return refuse(AbiError::BadHandle);
    if (!restXyz) return refuse(AbiError::NullPointer);
    Builder& B = it->second;
    const size_t n = static_cast<size_t>(B.vertexCount);
    B.rest.assign(restXyz, restXyz + n * 3);
    B.invMass.assign(invMass ? invMass : nullptr, invMass ? invMass + n : nullptr);
    B.statePos.assign(statePosXyz ? statePosXyz : nullptr, statePosXyz ? statePosXyz + n * 3 : nullptr);
    B.stateVel.assign(stateVelXyz ? stateVelXyz : nullptr, stateVelXyz ? stateVelXyz + n * 3 : nullptr);
    B.hasVertices = true;
    return 1;
}

int32_t aver_phys_sb_builder_faces(int32_t b, const int32_t* tri3, int32_t triCount) {
    const auto it = g_builders.find(b);
    if (it == g_builders.end()) return refuse(AbiError::BadHandle);
    if (!tri3 || triCount < 0) return refuse(AbiError::NullPointer);
    it->second.faces.assign(tri3, tri3 + static_cast<size_t>(triCount) * 3);
    return 1;
}

int32_t aver_phys_sb_builder_edges(int32_t b, const int32_t* ab, const float* restCm, const float* compliance, int32_t edgeCount) {
    const auto it = g_builders.find(b);
    if (it == g_builders.end()) return refuse(AbiError::BadHandle);
    if (edgeCount < 0 || (edgeCount > 0 && (!ab || !restCm))) return refuse(AbiError::NullPointer);
    Builder& B = it->second;
    const size_t n = static_cast<size_t>(edgeCount);
    B.edgeAb.assign(ab, ab + n * 2);
    B.edgeRest.assign(restCm, restCm + n);
    if (compliance) B.edgeCompliance.assign(compliance, compliance + n);
    else B.edgeCompliance.assign(n, 0.0f);
    return 1;
}

int32_t aver_phys_sb_builder_bends(int32_t b, const int32_t* quad4, const float* compliance, int32_t bendCount) {
    const auto it = g_builders.find(b);
    if (it == g_builders.end()) return refuse(AbiError::BadHandle);
    if (bendCount < 0 || (bendCount > 0 && !quad4)) return refuse(AbiError::NullPointer);
    Builder& B = it->second;
    const size_t n = static_cast<size_t>(bendCount);
    B.bendQuad.assign(quad4, quad4 + n * 4);
    if (compliance) B.bendCompliance.assign(compliance, compliance + n);
    else B.bendCompliance.assign(n, 0.0f);
    return 1;
}

int32_t aver_phys_sb_builder_skin(int32_t b, const int32_t* joints4, const float* weights4, const float* reachCm,
                                  float backstopCm, int32_t jointCount) {
    const auto it = g_builders.find(b);
    if (it == g_builders.end()) return refuse(AbiError::BadHandle);
    if (!joints4 || !weights4 || !reachCm) return refuse(AbiError::NullPointer);
    if (jointCount < 1) return refuse(AbiError::InvalidArgument);
    Builder& B = it->second;
    const size_t n = static_cast<size_t>(B.vertexCount);
    B.joints4.assign(joints4, joints4 + n * 4);
    B.weights4.assign(weights4, weights4 + n * 4);
    B.reach.assign(reachCm, reachCm + n);
    B.backstopCm = backstopCm;
    B.jointCount = jointCount;
    B.hasSkin = true;
    return 1;
}

int32_t aver_phys_sb_builder_params(int32_t b, const float* p, int32_t n) {
    const auto it = g_builders.find(b);
    if (it == g_builders.end()) return refuse(AbiError::BadHandle);
    if (!p || n < 0) return refuse(AbiError::NullPointer);
    overlayParams(it->second.params, p, n);
    return 1;
}

int32_t aver_phys_sb_builder_cancel(int32_t b) {
    softbody::sbxBuilderDrop(b);
    return g_builders.erase(b) ? 1 : refuse(AbiError::BadHandle);
}

int32_t aver_phys_sb_builder_finish(int32_t b, float cx, float cy, float cz, int32_t replaceBody, int32_t stateSrcBody,
                                    const int32_t* stateMap, int32_t* outEdgeSlot, int32_t* outBendSlot) {
    if (!g_world) return refuse(AbiError::NotInitialised);
    const auto bit = g_builders.find(b);
    if (bit == g_builders.end()) return refuse(AbiError::BadHandle);
    const Builder& B = bit->second;
    if (!B.hasVertices) return refuse(AbiError::InvalidArgument);
    const int32_t n = B.vertexCount;
    const size_t edgeCount = B.edgeAb.size() / 2, bendCount = B.bendQuad.size() / 4;

    const JPH::BodyID* oldId = nullptr;
    JPH::BodyID oldIdValue;
    if (replaceBody) {
        oldId = findBody(replaceBody);
        if (!oldId) return 0;
        oldIdValue = *oldId;
    }
    const JPH::BodyID* srcId = nullptr;
    if (stateSrcBody) {
        srcId = findBody(stateSrcBody);
        if (!srcId) return 0;
    }

    // ---- shared settings, built by hand: one Edge per input edge, one DihedralBend per bend ----
    const Vec3 centre(cx, cy, cz);
    const JPH::Vec3 jcentre = toJolt(centre);
    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
    shared->mVertices.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        const Vec3 r(B.rest[i * 3 + 0], B.rest[i * 3 + 1], B.rest[i * 3 + 2]);
        const JPH::Vec3 jp = toJolt(r - centre);
        JPH::SoftBodySharedSettings::Vertex v;
        v.mPosition = JPH::Float3(jp.GetX(), jp.GetY(), jp.GetZ());
        v.mInvMass = B.invMass.empty() ? 1.0f : B.invMass[static_cast<size_t>(i)];
        shared->mVertices.push_back(v);
    }
    // Faces are wound straight through, exactly as buildSoftShared does (see the comment there).
    for (size_t t = 0; t + 2 < B.faces.size(); t += 3) {
        const int32_t a = B.faces[t], bb = B.faces[t + 1], c = B.faces[t + 2];
        if (a < 0 || bb < 0 || c < 0 || a >= n || bb >= n || c >= n) return refuse(AbiError::OutOfRange);
        if (a == bb || bb == c || a == c) continue;
        shared->AddFace(JPH::SoftBodySharedSettings::Face(static_cast<JPH::uint32>(a), static_cast<JPH::uint32>(bb), static_cast<JPH::uint32>(c)));
    }
    // A body of rods, volumes or auto constraints may have no faces: sbxPrepare gives it one.
    if (shared->mFaces.empty() && !softbody::sbxFacelessOk(b)) return refuse(AbiError::InvalidArgument);
    for (size_t e = 0; e < edgeCount; ++e) {
        const int32_t a = B.edgeAb[e * 2], c = B.edgeAb[e * 2 + 1];
        if (a < 0 || c < 0 || a >= n || c >= n) return refuse(AbiError::OutOfRange);
        if (a == c) return refuse(AbiError::InvalidArgument);
        JPH::SoftBodySharedSettings::Edge edge(static_cast<JPH::uint32>(a), static_cast<JPH::uint32>(c), std::max(0.0f, B.edgeCompliance[e]));
        edge.mRestLength = cmToM(B.edgeRest[e]);
        shared->mEdgeConstraints.push_back(edge);
    }
    for (size_t k = 0; k < bendCount; ++k) {
        int32_t q[4];
        for (int j = 0; j < 4; ++j) {
            q[j] = B.bendQuad[k * 4 + static_cast<size_t>(j)];
            if (q[j] < 0 || q[j] >= n) return refuse(AbiError::OutOfRange);
        }
        for (int x = 0; x < 4; ++x)
            for (int y = x + 1; y < 4; ++y)
                if (q[x] == q[y]) return refuse(AbiError::InvalidArgument);
        shared->mDihedralBendConstraints.push_back(JPH::SoftBodySharedSettings::DihedralBend(
            static_cast<JPH::uint32>(q[0]), static_cast<JPH::uint32>(q[1]), static_cast<JPH::uint32>(q[2]),
            static_cast<JPH::uint32>(q[3]), std::max(0.0f, B.bendCompliance[k])));
    }
    if (bendCount) shared->CalculateBendConstraintConstants();

    if (B.hasSkin) {
        // ONE IDENTITY INVERSE BIND PER JOINT (the trick aver_phys_softbody_create_skinned documents): the
        // palette already carries model x inverse-bind, and the bind pose is the rest positions above.
        shared->mInvBindMatrices.reserve(static_cast<size_t>(B.jointCount));
        for (int32_t j = 0; j < B.jointCount; ++j)
            shared->mInvBindMatrices.push_back(JPH::SoftBodySharedSettings::InvBind(static_cast<JPH::uint32>(j), JPH::Mat44::sIdentity()));
        const float backstop = B.backstopCm < 0.0f ? FLT_MAX : cmToM(B.backstopCm);
        for (int32_t i = 0; i < n; ++i) {
            const float reach = B.reach[static_cast<size_t>(i)];
            if (reach < 0.0f) continue;
            JPH::SoftBodySharedSettings::Skinned sk(static_cast<JPH::uint32>(i), cmToM(reach), backstop, 0.0f);
            int slot = 0;
            for (int k = 0; k < 4; ++k) {
                const int32_t joint = B.joints4[static_cast<size_t>(i) * 4 + static_cast<size_t>(k)];
                const float w = B.weights4[static_cast<size_t>(i) * 4 + static_cast<size_t>(k)];
                // A zero weight ends Jolt's per-vertex list, so zeros are compacted out here.
                if (w <= 0.0f || joint < 0 || joint >= B.jointCount) continue;
                sk.mWeights[slot++] = JPH::SoftBodySharedSettings::SkinWeight(static_cast<JPH::uint32>(joint), w);
            }
            if (slot == 0) continue;   // an unweighted vertex is left free rather than pinned to joint 0
            sk.NormalizeWeights();
            shared->mSkinnedConstraints.push_back(sk);
        }
        if (!shared->mSkinnedConstraints.empty()) shared->CalculateSkinnedConstraintNormals();
    }
    // Volumes, long range attachments, rods and Jolt's own constraint generator (physics_softbody_ext_abi.h).
    if (const AbiError why = softbody::sbxPrepare(b, *shared, edgeCount > 0 || bendCount > 0); why != AbiError::Ok) return refuse(why);
    JPH::SoftBodySharedSettings::OptimizationResults results;
    shared->Optimize(results);

    // ---- params: the replaced body's, then this builder's non-NaN entries ----
    SoftRecord* oldRec = replaceBody ? findRecord(replaceBody) : nullptr;
    float p[kParamCount];
    defaultParams(p);
    if (oldRec) std::memcpy(p, oldRec->params, sizeof p);
    overlayParams(p, B.params, kParamCount);

    uint64_t userData = static_cast<uint64_t>(static_cast<int64_t>(std::lround(p[AVER_PHYS_SB_P_ENTITY])));
    if (userData == 0 && oldId) {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), oldIdValue);
        if (lock.Succeeded()) userData = lock.GetBody().GetUserData();
    }

    JPH::SoftBodyCreationSettings s(shared, jcentre, JPH::Quat::sIdentity(), Layers::MOVING);
    paramsToCreation(s, p);
    s.mUserData = userData;
    JPH::Body* nb = g_world->system.GetBodyInterface().CreateSoftBody(s);
    if (!nb) {
        AVER_WARN("[Physics] soft body limit reached");
        return refuse(AbiError::AllocationFailed);
    }
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(nb->GetMotionProperties());
    auto& nv = mp->GetVertices();

    // ---- the starting state: carried from a body, or the builder's, or rest ----
    // Initialize copied the settings' rest positions, so the rest case needs nothing; writing positions
    // before the first step is safe (Jolt's warning is about missed collisions DURING simulation).
    if (srcId) {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *srcId);
        bool ok = lock.Succeeded() && lock.GetBody().IsSoftBody();
        if (ok) {
            const JPH::Body& sb = lock.GetBody();
            const auto& sv = static_cast<const JPH::SoftBodyMotionProperties*>(sb.GetMotionProperties())->GetVertices();
            if (!stateMap && static_cast<int32_t>(sv.size()) != n) ok = false;
            if (ok) {
                const JPH::RMat44 scom = sb.GetCenterOfMassTransform();
                for (int32_t i = 0; i < n; ++i) {
                    const int32_t j = stateMap ? stateMap[i] : i;
                    if (j < 0 || j >= static_cast<int32_t>(sv.size())) continue;
                    const JPH::Vec3 world = JPH::Vec3(scom * sv[static_cast<size_t>(j)].mPosition);
                    nv[static_cast<size_t>(i)].mPosition = world - jcentre;
                    nv[static_cast<size_t>(i)].mPreviousPosition = nv[static_cast<size_t>(i)].mPosition;
                    nv[static_cast<size_t>(i)].mVelocity = scom.Multiply3x3(sv[static_cast<size_t>(j)].mVelocity);
                }
            }
        }
        if (!ok) {
            g_world->system.GetBodyInterface().DestroyBody(nb->GetID());
            return refuse(AbiError::InvalidArgument);
        }
    } else {
        if (!B.statePos.empty())
            for (int32_t i = 0; i < n; ++i) {
                const Vec3 sp(B.statePos[i * 3 + 0], B.statePos[i * 3 + 1], B.statePos[i * 3 + 2]);
                nv[static_cast<size_t>(i)].mPosition = toJolt(sp - centre);
                nv[static_cast<size_t>(i)].mPreviousPosition = nv[static_cast<size_t>(i)].mPosition;
            }
        if (!B.stateVel.empty())
            for (int32_t i = 0; i < n; ++i)
                nv[static_cast<size_t>(i)].mVelocity = toJoltDir(Vec3(B.stateVel[i * 3 + 0], B.stateVel[i * 3 + 1], B.stateVel[i * 3 + 2]));
    }

    // ---- the registry record, keeping the replaced body's palette and aero ----
    SoftRecord rec;
    rec.settings = shared;
    rec.vertexCount = n;
    rec.jointCount = B.hasSkin ? B.jointCount : 0;
    std::memcpy(rec.params, p, sizeof p);
    rec.impacts = p[AVER_PHYS_SB_P_IMPACTS] >= 0.5f;
    if (oldRec) {
        rec.drag = oldRec->drag; rec.lift = oldRec->lift; rec.aeroFlags = oldRec->aeroFlags;
        rec.frozen = false;
        if (oldRec->paletteJoints >= rec.jointCount) {
            rec.palette = oldRec->palette;
            rec.paletteJoints = oldRec->paletteJoints;
            std::memcpy(rec.entityWorld, oldRec->entityWorld, sizeof rec.entityWorld);
            rec.hasEntityWorld = oldRec->hasEntityWorld;
        }
        softbody::sbxCarry(*oldRec, rec);   // policy, buoyancy, pairs, attachments and pressure survive a tear rebuild
    }
    softbody::sbxApply(b, *nb, *mp, rec, results);   // the builder's wave 11 params, pressure, skin, group, slots
    if (!shared->mSkinnedConstraints.empty()) {
        // Skin state starts at zero; without a palette the pull would be toward the origin. An identity
        // palette is the bind pose (the rest positions) until the host pushes a real one.
        if (rec.palette.empty()) {
            rec.paletteJoints = rec.jointCount;
            rec.palette.assign(static_cast<size_t>(rec.jointCount) * 16, 0.0f);
            for (int32_t j = 0; j < rec.jointCount; ++j)
                for (int d = 0; d < 4; ++d) rec.palette[static_cast<size_t>(j) * 16 + static_cast<size_t>(d) * 5] = 1.0f;
        }
        // Twice: a skin pass copies the previous skin position from the last one, and the first has none (Jolt starts it NaN).
        // A body the host does not skin before its first step (a frozen piece rebuilt by a tear) would solve against NaN.
        pushPalette(*nb, *mp, rec.palette.data(), rec.paletteJoints, rec.hasEntityWorld ? rec.entityWorld : nullptr, false);
        pushPalette(*nb, *mp, rec.palette.data(), rec.paletteJoints, rec.hasEntityWorld ? rec.entityWorld : nullptr, false);
    }

    // ---- swap in ----
    g_world->system.GetBodyInterface().AddBody(nb->GetID(), JPH::EActivation::Activate);
    int32_t handle;
    if (replaceBody) {
        handle = replaceBody;
        g_world->bodies[handle] = nb->GetID();
        g_world->byId.erase(oldIdValue);
        g_world->byId.emplace(nb->GetID(), handle);
        g_world->system.GetBodyInterface().RemoveBody(oldIdValue);
        g_world->system.GetBodyInterface().DestroyBody(oldIdValue);
    } else {
        handle = g_world->nextHandle++;
        g_world->bodies.emplace(handle, nb->GetID());
        g_world->byId.emplace(nb->GetID(), handle);
    }
    g_records[handle] = std::move(rec);

    if (outEdgeSlot)
        for (size_t e = 0; e < edgeCount; ++e) outEdgeSlot[e] = static_cast<int32_t>(results.mEdgeRemap[e]);
    if (outBendSlot)
        for (size_t k = 0; k < bendCount; ++k) outBendSlot[k] = static_cast<int32_t>(results.mDihedralBendRemap[k]);
    g_builders.erase(bit);
    softbody::sbxBuilderDrop(b);
    return handle;
}

int32_t aver_phys_sb_state(int32_t body, float* outPosXyz, float* outVelXyz, int32_t maxVertices) {
    if (maxVertices <= 0) return refuse(AbiError::InvalidArgument);
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!findRecord(body)) return refuse(AbiError::BadHandle);
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    const JPH::Body& b = lock.GetBody();
    const auto& verts = static_cast<const JPH::SoftBodyMotionProperties*>(b.GetMotionProperties())->GetVertices();
    const JPH::RMat44 com = b.GetCenterOfMassTransform();
    const int32_t n = std::min(maxVertices, static_cast<int32_t>(verts.size()));
    for (int32_t i = 0; i < n; ++i) {
        const JPH::SoftBodyVertex& v = verts[static_cast<size_t>(i)];
        if (outPosXyz) writeVec(outPosXyz + i * 3, fromJolt(JPH::Vec3(com * v.mPosition)));
        if (outVelXyz) writeVec(outVelXyz + i * 3, fromJoltDir(com.Multiply3x3(v.mVelocity)));
    }
    return n;
}

int32_t aver_phys_sb_vertex_count(int32_t body) {
    const SoftRecord* r = findRecord(body);
    return r ? r->vertexCount : 0;
}

int32_t aver_phys_sb_set_edge_rest(int32_t body, const int32_t* edgeSlot, const float* restCm, int32_t n) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (n < 0 || (n > 0 && (!edgeSlot || !restCm))) return refuse(AbiError::NullPointer);
    const int32_t edges = static_cast<int32_t>(r->settings->mEdgeConstraints.size());
    for (int32_t i = 0; i < n; ++i)
        if (edgeSlot[i] < 0 || edgeSlot[i] >= edges || !(restCm[i] >= 0.0f)) return refuse(AbiError::OutOfRange);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return refuse(AbiError::BadHandle);
    for (int32_t i = 0; i < n; ++i) r->settings->mEdgeConstraints[static_cast<size_t>(edgeSlot[i])].mRestLength = cmToM(restCm[i]);
    r->frozen = false;
    g_world->system.GetBodyInterfaceNoLock().ActivateBody(*id);
    return 1;
}

int32_t aver_phys_sb_rest_bends_from_pose(int32_t body, const int32_t* bendSlot, int32_t n) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (n < 0 || (n > 0 && !bendSlot)) return refuse(AbiError::NullPointer);
    const int32_t bends = static_cast<int32_t>(r->settings->mDihedralBendConstraints.size());
    for (int32_t i = 0; i < n; ++i)
        if (bendSlot[i] < 0 || bendSlot[i] >= bends) return refuse(AbiError::OutOfRange);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    const auto& verts = mp->GetVertices();
    for (int32_t i = 0; i < n; ++i) {
        JPH::SoftBodySharedSettings::DihedralBend& bend = r->settings->mDihedralBendConstraints[static_cast<size_t>(bendSlot[i])];
        // The signed angle exactly as ApplyDihedralBendConstraints computes it, so the residual is zero.
        const JPH::Vec3 x0 = verts[bend.mVertex[0]].mPosition, x1 = verts[bend.mVertex[1]].mPosition;
        const JPH::Vec3 x2 = verts[bend.mVertex[2]].mPosition, x3 = verts[bend.mVertex[3]].mPosition;
        const JPH::Vec3 e = x1 - x0;
        if (e.Length() < 1.0e-6f) continue;
        const JPH::Vec3 n1 = (x2 - x0).Cross(x2 - x1);
        const JPH::Vec3 n2 = (x3 - x1).Cross(x3 - x0);
        const float denom = JPH::Sqrt(n1.LengthSq() * n2.LengthSq());
        if (denom < 1.0e-12f) continue;
        const float sign = JPH::Sign(n2.Cross(n1).Dot(e));
        bend.mInitialAngle = sign * JPH::ACosApproximate(n1.Dot(n2) / denom);
    }
    r->frozen = false;
    g_world->system.GetBodyInterfaceNoLock().ActivateBody(*id);
    return 1;
}

int32_t aver_phys_sb_impulse(int32_t body, const float* centreCm, float radiusCm, const float* dirUnit,
                             float speedCmS, float radialBlend, float falloffPower) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (!centreCm || !dirUnit) return refuse(AbiError::NullPointer);
    if (!(radiusCm > 0.0f)) return refuse(AbiError::InvalidArgument);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    JPH::Body& b = lock.GetBody();
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
    const JPH::RMat44 com = b.GetCenterOfMassTransform();
    const Vec3 centre(centreCm[0], centreCm[1], centreCm[2]);
    const Vec3 dir = Vec3(dirUnit[0], dirUnit[1], dirUnit[2]).getSafeNormal();
    const float blend = std::clamp(radialBlend, 0.0f, 1.0f);
    const float power = std::max(0.0f, falloffPower);
    for (JPH::SoftBodyVertex& v : mp->GetVertices()) {
        if (v.mInvMass <= 0.0f) continue;   // kinematic
        const Vec3 world = fromJolt(JPH::Vec3(com * v.mPosition));
        const Vec3 rel = world - centre;
        const float d = rel.size();
        if (d >= radiusCm) continue;
        const Vec3 radial = d > 1.0e-4f ? rel / d : dir;
        const Vec3 along = (dir * (1.0f - blend) + radial * blend).getSafeNormal();
        const float w = std::pow(1.0f - d / radiusCm, power);
        // Velocity is a free vector: only the rotation of the COM transform applies, inverted.
        v.mVelocity += com.GetRotation().Multiply3x3Transposed(toJoltDir(along * (speedCmS * w)));
    }
    r->frozen = false;
    g_world->system.GetBodyInterfaceNoLock().ActivateBody(*id);   // see aver_phys_softbody_apply_impulse on why no-lock
    return 1;
}

int32_t aver_phys_sb_abi_version(void) { return AVER_PHYS_SB_ABI_VERSION; }

int32_t aver_phys_sb_add_velocity(int32_t body, const int32_t* vertex, const float* dvXyz, int32_t n) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (n < 0 || (n > 0 && (!vertex || !dvXyz))) return refuse(AbiError::NullPointer);
    for (int32_t i = 0; i < n; ++i) {
        if (vertex[i] < 0 || vertex[i] >= r->vertexCount) return refuse(AbiError::OutOfRange);
        if (!std::isfinite(dvXyz[i * 3]) || !std::isfinite(dvXyz[i * 3 + 1]) || !std::isfinite(dvXyz[i * 3 + 2])) return refuse(AbiError::InvalidArgument);
    }
    if (n == 0) return 1;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    JPH::Body& b = lock.GetBody();
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
    const JPH::RMat44 com = b.GetCenterOfMassTransform();
    auto& verts = mp->GetVertices();
    bool any = false;
    for (int32_t i = 0; i < n; ++i) {
        const Vec3 dv(dvXyz[i * 3], dvXyz[i * 3 + 1], dvXyz[i * 3 + 2]);
        JPH::SoftBodyVertex& v = verts[static_cast<size_t>(vertex[i])];
        if (v.mInvMass <= 0.0f) continue;   // kinematic
        if (dv.x == 0.0f && dv.y == 0.0f && dv.z == 0.0f) continue;
        // Velocity is a free vector: only the rotation of the COM transform applies, inverted.
        v.mVelocity += com.GetRotation().Multiply3x3Transposed(toJoltDir(dv));
        any = true;
    }
    if (any) {
        r->frozen = false;
        g_world->system.GetBodyInterfaceNoLock().ActivateBody(*id);   // see aver_phys_sb_impulse on why no-lock
    }
    return 1;
}

int32_t aver_phys_sb_skin_world(int32_t body, const float* jointMatrices, int32_t jointCount, const float* entityWorld16, int32_t hardSkin) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (!jointMatrices) return refuse(AbiError::NullPointer);
    if (jointCount < r->jointCount || jointCount < 1) return refuse(AbiError::OutOfRange);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    JPH::Body& b = lock.GetBody();
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
    if (r->settings->mSkinnedConstraints.empty()) return refuse(AbiError::Unsupported);
    pushPalette(b, *mp, jointMatrices, jointCount, entityWorld16, hardSkin != 0);
    r->palette.assign(jointMatrices, jointMatrices + static_cast<size_t>(jointCount) * 16);
    r->paletteJoints = jointCount;
    r->hasEntityWorld = entityWorld16 != nullptr;
    if (entityWorld16) std::memcpy(r->entityWorld, entityWorld16, sizeof r->entityWorld);
    return 1;
}

int32_t aver_phys_sb_set_active(int32_t body, int32_t active) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (active) {
        r->frozen = false;
        bi().ActivateBody(*id);
    } else {
        r->frozen = true;
        bi().DeactivateBody(*id);
    }
    return 1;
}

int32_t aver_phys_sb_is_active(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !findRecord(body)) return 0;
    return bi().IsActive(*id) ? 1 : 0;
}

int32_t aver_phys_sb_set_params(int32_t body, const float* p, int32_t n) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (!p || n < 0) return refuse(AbiError::NullPointer);
    overlayParams(r->params, p, n);
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return refuse(AbiError::BadHandle);
    JPH::Body& b = lock.GetBody();
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
    const float* q = r->params;
    mp->SetNumIterations(static_cast<JPH::uint32>(std::clamp(static_cast<int>(std::lround(q[AVER_PHYS_SB_P_ITERATIONS])), 1, 64)));
    mp->SetLinearDamping(std::max(0.0f, q[AVER_PHYS_SB_P_DAMPING]));
    b.SetFriction(std::max(0.0f, q[AVER_PHYS_SB_P_FRICTION]));
    b.SetRestitution(std::clamp(q[AVER_PHYS_SB_P_RESTITUTION], 0.0f, 1.0f));
    mp->SetGravityFactor(q[AVER_PHYS_SB_P_GRAVITY_SCALE]);
    mp->SetVertexRadius(cmToM(std::max(0.0f, q[AVER_PHYS_SB_P_VERTEX_RADIUS_CM])));
    mp->SetMaxLinearVelocity(cmToM(std::max(1.0f, q[AVER_PHYS_SB_P_MAX_SPEED_CMS])));
    if (!r->sleepForcedOff) b.SetAllowSleeping(q[AVER_PHYS_SB_P_ALLOW_SLEEP] >= 0.5f);
    mp->SetFacesDoubleSided(q[AVER_PHYS_SB_P_DOUBLE_SIDED] >= 0.5f);
    b.SetUserData(static_cast<uint64_t>(static_cast<int64_t>(std::lround(q[AVER_PHYS_SB_P_ENTITY]))));
    r->impacts = q[AVER_PHYS_SB_P_IMPACTS] >= 0.5f;
    // The layer is a body-interface write (it re-files the body in the broad phase), so after the lock.
    const u32 layer = static_cast<u32>(clampLayer(q[AVER_PHYS_SB_P_LAYER]));
    lock.ReleaseLock();
    bi().SetObjectLayer(*id, Layers::encode(layer, true));
    return 1;
}

int32_t aver_phys_sb_count(void) { return static_cast<int32_t>(g_records.size()); }

int32_t aver_phys_sb_set_aero(int32_t body, float drag, float lift, int32_t flags) {
    if (!findBody(body)) return 0;
    SoftRecord* r = findRecord(body);
    if (!r) return refuse(AbiError::BadHandle);
    if (!std::isfinite(drag) || !std::isfinite(lift)) return refuse(AbiError::InvalidArgument);
    r->drag = std::clamp(drag, 0.0f, 2.0f);
    r->lift = std::clamp(lift, 0.0f, 2.0f);
    r->aeroFlags = flags;
    return 1;
}

void aver_phys_sb_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence) {
    softbody::WindParams w;
    w.base = Vec3(vx, vy, vz);
    w.gustSpeed = gustSpeed;
    w.gustHz = gustHz;
    w.turbulence = std::max(0.0f, turbulence);
    if (!std::isfinite(w.base.x) || !std::isfinite(w.base.y) || !std::isfinite(w.base.z) || !std::isfinite(gustSpeed) ||
        !std::isfinite(gustHz) || !std::isfinite(turbulence)) return;
    g_wind = w;
}

int32_t aver_phys_sb_impact_count(void) { return static_cast<int32_t>(g_events.size()); }

int32_t aver_phys_sb_impact_get(int32_t index, int32_t* outSoftBody, int32_t* outOtherBody, float* outPoint3, float* outNormal3,
                                float* outClosingCmS, float* outOtherMassKg, int32_t* outContactVertices) {
    if (index < 0 || index >= static_cast<int32_t>(g_events.size())) return refuse(AbiError::OutOfRange);
    const ImpactEvent& e = g_events[static_cast<size_t>(index)];
    if (outSoftBody) *outSoftBody = e.soft;
    if (outOtherBody) *outOtherBody = e.other;
    if (outPoint3) std::memcpy(outPoint3, e.point, sizeof e.point);
    if (outNormal3) std::memcpy(outNormal3, e.normal, sizeof e.normal);
    if (outClosingCmS) *outClosingCmS = e.closingCmS;
    if (outOtherMassKg) *outOtherMassKg = e.otherMassKg;
    if (outContactVertices) *outContactVertices = e.vertices;
    return 1;
}

} // extern "C"
