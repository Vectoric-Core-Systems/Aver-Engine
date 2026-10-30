// PhysicsSurface.cpp -- the process-wide table of interned surface materials, the registry on top of it, the combine
// hook, the body label, and the detail queries (physics_surface_abi.h). Not cleared by aver_phys_shutdown: shapes
// outlive worlds, and so do the materials they point at.
#include "PhysicsSurface.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsQueryFilters.hpp"

#include "aver/physics/physics_queries_abi.h"
#include "aver/physics/physics_surface_abi.h"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"

#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::physics::detail {

const char* const AverSurface::kName = "AverSurface";

namespace {

std::mutex g_surfaceMutex;
std::unordered_map<int32_t, JPH::Ref<AverSurface>> g_surfaces;
std::unordered_map<int32_t, std::string> g_names;   // registered ids only
std::vector<int32_t> g_order;                        // registered ids in registration order
std::atomic<bool> g_interned{false};

AverSurface* internLocked(int32_t surface) {
    auto it = g_surfaces.find(surface);
    if (it == g_surfaces.end()) {
        if (!JPH::Allocate) JPH::RegisterDefaultAllocator();   // the registry works before aver_phys_init (the project's table loads first)
        it = g_surfaces.emplace(surface, JPH::Ref<AverSurface>(new AverSurface(surface))).first;
        g_interned.store(true, std::memory_order_relaxed);
    }
    return it->second.GetPtr();
}

// A number a caller gave: below 0 defers to the body, above 10 clamps.
float normNumber(float v) { return v < 0.0f ? -1.0f : std::min(v, 10.0f); }

int32_t surfFail(aver::AbiError e) {
    aver::setAbiError(e);
    return 0;
}
int32_t surfOk(int32_t v) {
    aver::setAbiError(aver::AbiError::Ok);
    return v;
}

// The AverSurface a body's leaf carries at `sub`, or null (none, not ours, or a soft body, which has no leaf materials).
const AverSurface* leafSurface(const JPH::Body& body, const JPH::SubShapeID& sub) {
    if (body.IsSoftBody()) return nullptr;
    const JPH::Shape* shape = body.GetShape();
    if (!shape) return nullptr;
    const JPH::PhysicsMaterial* m = shape->GetMaterial(sub);
    if (!m || m->GetDebugName() != AverSurface::kName) return nullptr;
    return static_cast<const AverSurface*>(m);
}

// Jolt's own combine expressions, with the leaf numbers in place of the body's where a surface carries one. While no
// surface has ever been interned they ARE Jolt's expressions on the body values.
// With the combine mode on the material rule (physics_surface_abi.h) the same per-side numbers meet in its expressions instead.
bool materialCombine() { return g_world && g_world->combineMode == AVER_PHYS_COMBINE_MATERIAL; }

uint8_t combineFlagsOf(const JPH::Body& body) {
    if (g_world->combineFlags.empty()) return 0;
    const auto h = g_world->byId.find(body.GetID());
    if (h == g_world->byId.end()) return 0;
    const auto f = g_world->combineFlags.find(h->second);
    return f == g_world->combineFlags.end() ? uint8_t{0} : f->second;
}

float combineFriction(const JPH::Body& b1, const JPH::SubShapeID& s1, const JPH::Body& b2, const JPH::SubShapeID& s2) {
    if (materialCombine())
        return combineFrictionValues(surfaceFrictionOf(b1, s1, b1.GetFriction()), b1, surfaceFrictionOf(b2, s2, b2.GetFriction()), b2);
    if (!g_interned.load(std::memory_order_relaxed)) return JPH::Sqrt(b1.GetFriction() * b2.GetFriction());
    return JPH::Sqrt(surfaceFrictionOf(b1, s1, b1.GetFriction()) * surfaceFrictionOf(b2, s2, b2.GetFriction()));
}
float combineRestitution(const JPH::Body& b1, const JPH::SubShapeID& s1, const JPH::Body& b2, const JPH::SubShapeID& s2) {
    if (materialCombine())
        return combineRestitutionValues(surfaceRestitutionOf(b1, s1, b1.GetRestitution()), b1, surfaceRestitutionOf(b2, s2, b2.GetRestitution()), b2);
    if (!g_interned.load(std::memory_order_relaxed)) return std::max(b1.GetRestitution(), b2.GetRestitution());
    return std::max(surfaceRestitutionOf(b1, s1, b1.GetRestitution()), surfaceRestitutionOf(b2, s2, b2.GetRestitution()));
}

// The surface a (body, sub-shape) names: the leaf material, else the body label, else 0.
int32_t resolvedSurfaceId(const JPH::Body& body, const JPH::SubShapeID& sub) {
    if (body.IsSoftBody()) return 0;   // a soft body writes its whole user word (the entity): it has no label half
    if (const AverSurface* s = leafSurface(body, sub)) return s->id;
    return bodyLabelOf(body.GetUserData());
}

// ---- sub-shape detail -------------------------------------------------------------------------------------------

struct SubShapeDetail {
    int32_t sub = 0, surface = 0, tag = 0, child = -1, childUser = 0, triangle = -1, cellX = -1, cellY = -1;
    float friction = -1.0f, restitution = -1.0f;
};

// What `id` names inside `body`'s shape: the compound child it lies in, the mesh triangle's user data, the
// heightfield cell, and the surface with the numbers it carries. Decorators pass the id through unchanged; the first
// compound met is the "top-level" one the child index refers to.
SubShapeDetail describeSubShape(const JPH::Body& body, JPH::SubShapeID id) {
    SubShapeDetail d;
    d.sub = static_cast<int32_t>(id.GetValue());
    const AverSurface* obj = nullptr;

    const JPH::Shape* s = body.IsSoftBody() ? nullptr : body.GetShape();
    JPH::SubShapeID rem = id;
    for (int depth = 0; s && depth < 16; ++depth) {
        switch (s->GetSubType()) {
            case JPH::EShapeSubType::Scaled:
            case JPH::EShapeSubType::RotatedTranslated:
            case JPH::EShapeSubType::OffsetCenterOfMass:
                s = static_cast<const JPH::DecoratedShape*>(s)->GetInnerShape();
                continue;
            case JPH::EShapeSubType::StaticCompound:
            case JPH::EShapeSubType::MutableCompound: {
                const auto* c = static_cast<const JPH::CompoundShape*>(s);
                if (!c->IsSubShapeIDValid(rem)) { s = nullptr; continue; }   // a stale or invented id
                JPH::SubShapeID next;
                const uint32_t idx = c->GetSubShapeIndexFromID(rem, next);
                const JPH::CompoundShape::SubShape& child = c->GetSubShape(idx);
                if (d.child < 0) {
                    d.child = static_cast<int32_t>(idx);
                    d.childUser = static_cast<int32_t>(child.mUserData);
                }
                s = child.mShape.GetPtr();
                rem = next;
                continue;
            }
            default:
                break;
        }
        break;
    }

    if (s) {
        if (s->GetSubType() == JPH::EShapeSubType::Mesh) {
            if (s->GetUserData() == kMeshTriangleUserData)
                d.triangle = static_cast<int32_t>(static_cast<const JPH::MeshShape*>(s)->GetTriangleUserData(rem));
        } else if (s->GetSubType() == JPH::EShapeSubType::HeightField) {
            JPH::uint x = 0, y = 0, tri = 0;
            static_cast<const JPH::HeightFieldShape*>(s)->GetSubShapeCoordinates(rem, x, y, tri);
            d.cellX = static_cast<int32_t>(x);
            d.cellY = static_cast<int32_t>(y);
        }
        const JPH::PhysicsMaterial* m = s->GetMaterial(rem);
        if (m && m->GetDebugName() == AverSurface::kName) obj = static_cast<const AverSurface*>(m);
    }

    if (obj) {
        d.surface = obj->id;
    } else if (const int32_t label = body.IsSoftBody() ? 0 : bodyLabelOf(body.GetUserData()); label != 0) {
        d.surface = label;
        obj = surfaceObject(label);
    }
    if (obj) {
        d.friction = obj->friction.load(std::memory_order_relaxed);
        d.restitution = obj->restitution.load(std::memory_order_relaxed);
        d.tag = obj->tag.load(std::memory_order_relaxed);
    }
    return d;
}

// ---- output blocks ------------------------------------------------------------------------------------------------

struct HitBlocks {
    float f[AVER_PHYS_HITF_COUNT] = {};
    int32_t i[AVER_PHYS_HITI_COUNT] = {};
};

void fillHit(HitBlocks& h, const JPH::Body& body, int32_t bodyHandle, JPH::SubShapeID sub) {
    const SubShapeDetail d = describeSubShape(body, sub);
    h.f[AVER_PHYS_HITF_FRICTION] = d.friction;
    h.f[AVER_PHYS_HITF_RESTITUTION] = d.restitution;
    h.i[AVER_PHYS_HITI_BODY] = bodyHandle;
    h.i[AVER_PHYS_HITI_ENTITY] = static_cast<int32_t>(body.GetUserData());
    h.i[AVER_PHYS_HITI_SUB_SHAPE] = d.sub;
    h.i[AVER_PHYS_HITI_SURFACE] = d.surface;
    h.i[AVER_PHYS_HITI_TAG] = d.tag;
    h.i[AVER_PHYS_HITI_CHILD] = d.child;
    h.i[AVER_PHYS_HITI_CHILD_USER_DATA] = d.childUser;
    h.i[AVER_PHYS_HITI_TRIANGLE] = d.triangle;
    h.i[AVER_PHYS_HITI_CELL_X] = d.cellX;
    h.i[AVER_PHYS_HITI_CELL_Y] = d.cellY;
}

void setPoint(HitBlocks& h, const Vec3& p, const Vec3& n, float distanceCm) {
    h.f[AVER_PHYS_HITF_POINT_X] = p.x; h.f[AVER_PHYS_HITF_POINT_Y] = p.y; h.f[AVER_PHYS_HITF_POINT_Z] = p.z;
    h.f[AVER_PHYS_HITF_NORMAL_X] = n.x; h.f[AVER_PHYS_HITF_NORMAL_Y] = n.y; h.f[AVER_PHYS_HITF_NORMAL_Z] = n.z;
    h.f[AVER_PHYS_HITF_DISTANCE_CM] = distanceCm;
}

// Capacity check shared by every detail call: false (reason recorded) for a negative capacity or a missing buffer.
bool blocksOk(const void* outF, int32_t nF, const void* outI, int32_t nI) {
    if (nF < 0 || nI < 0) { aver::setAbiError(aver::AbiError::InvalidArgument); return false; }
    if ((nF > 0 && !outF) || (nI > 0 && !outI)) { aver::setAbiError(aver::AbiError::NullPointer); return false; }
    return true;
}

template <size_t NF, size_t NI>
void writeBlocks(const float (&f)[NF], const int32_t (&iv)[NI], float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    for (int32_t k = 0; k < std::min<int32_t>(nF, static_cast<int32_t>(NF)); ++k) outF[k] = f[k];
    for (int32_t k = 0; k < std::min<int32_t>(nI, static_cast<int32_t>(NI)); ++k) outI[k] = iv[k];
}

// The Jolt id behind a body OR character handle (a character's inner body), or an invalid id.
JPH::BodyID bodyIdOfHandle(int32_t handle) {
    if (const JPH::BodyID* id = findBody(handle)) return *id;
    if (const JPH::CharacterVirtual* c = findCharacter(handle)) {
        aver::setAbiError(aver::AbiError::Ok);
        return c->GetInnerBodyID();
    }
    return JPH::BodyID();
}

} // namespace

// ---- interning (the builders' side) ---------------------------------------------------------------------------------

JPH::RefConst<JPH::PhysicsMaterial> surfaceMaterial(int32_t surface) {
    if (surface == 0) return JPH::RefConst<JPH::PhysicsMaterial>();
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    return JPH::RefConst<JPH::PhysicsMaterial>(internLocked(surface));
}

int32_t surfaceIdOf(const JPH::PhysicsMaterial* material) {
    if (!material || material->GetDebugName() != AverSurface::kName) return 0;
    return static_cast<const AverSurface*>(material)->id;
}

bool surfacesInterned() { return g_interned.load(std::memory_order_relaxed); }

const AverSurface* surfaceObject(int32_t surface) {
    if (surface == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    const auto it = g_surfaces.find(surface);
    return it == g_surfaces.end() ? nullptr : it->second.GetPtr();
}

// ---- the combine hook and the contact record -------------------------------------------------------------------------

float surfaceFrictionOf(const JPH::Body& body, const JPH::SubShapeID& sub, float fallback) {
    if (!g_interned.load(std::memory_order_relaxed)) return fallback;
    if (const AverSurface* s = leafSurface(body, sub)) {
        const float f = s->friction.load(std::memory_order_relaxed);
        if (f >= 0.0f) return f;
    }
    return fallback;
}

float surfaceRestitutionOf(const JPH::Body& body, const JPH::SubShapeID& sub, float fallback) {
    if (!g_interned.load(std::memory_order_relaxed)) return fallback;
    if (const AverSurface* s = leafSurface(body, sub)) {
        const float r = s->restitution.load(std::memory_order_relaxed);
        if (r >= 0.0f) return r;
    }
    return fallback;
}

float combineFrictionValues(float fa, const JPH::Body& a, float fb, const JPH::Body& b) {
    if (!materialCombine()) return std::sqrt(fa * fb);
    const bool rough = ((combineFlagsOf(a) | combineFlagsOf(b)) & AVER_PHYS_BODY_COMBINE_ROUGH) != 0;
    return rough ? std::max(fa, fb) : std::min(fa, fb);
}

float combineRestitutionValues(float ra, const JPH::Body& a, float rb, const JPH::Body& b) {
    if (!materialCombine()) return std::max(ra, rb);
    const float sa = (combineFlagsOf(a) & AVER_PHYS_BODY_COMBINE_ABSORBENT) ? -ra : ra;
    const float sb = (combineFlagsOf(b) & AVER_PHYS_BODY_COMBINE_ABSORBENT) ? -rb : rb;
    return std::clamp(sa + sb, 0.0f, 1.0f);
}

void installSurfaceCombine(JPH::PhysicsSystem& system) {
    system.SetCombineFriction(&combineFriction);
    system.SetCombineRestitution(&combineRestitution);
}

void fillContactDetail(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                       const JPH::ContactSettings& settings, ContactEvent& out) {
    out.subA = static_cast<int32_t>(manifold.mSubShapeID1.GetValue());
    out.subB = static_cast<int32_t>(manifold.mSubShapeID2.GetValue());
    out.friction = settings.mCombinedFriction;
    out.restitution = settings.mCombinedRestitution;
    // The normal points from body 1 to body 2, so a positive relative speed along it is the bodies closing.
    const JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
    out.closing = mToCm((a.GetPointVelocity(p) - b.GetPointVelocity(p)).Dot(manifold.mWorldSpaceNormal));
    if (surfacesInterned()) {
        out.surfA = resolvedSurfaceId(a, manifold.mSubShapeID1);
        out.surfB = resolvedSurfaceId(b, manifold.mSubShapeID2);
    }
}

} // namespace aver::physics::detail

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

extern "C" {

int32_t aver_phys_surface_abi_version(void) { return AVER_PHYS_SURFACE_ABI_VERSION; }

// ---- identity and registry -----------------------------------------------------------------------------------------

int32_t aver_phys_surface_id(const char* utf8Name) {
    return utf8Name ? aver::surfaceIdFromName(utf8Name) : 0;
}

int32_t aver_phys_surface_register(const char* utf8Name, float friction, float restitution, int32_t tag) {
    if (!utf8Name || !*utf8Name) return surfFail(AbiError::InvalidArgument);
    if (!std::isfinite(friction) || !std::isfinite(restitution)) return surfFail(AbiError::InvalidArgument);
    const std::string name = utf8Name;
    const int32_t id = aver::surfaceIdFromName(name);
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    const auto known = g_names.find(id);
    if (known != g_names.end() && known->second != name) {
        AVER_WARN("[Physics] surface '{}' hashes to the same id as the registered '{}': refused", name, known->second);
        return surfFail(AbiError::InvalidArgument);
    }
    AverSurface* s = internLocked(id);
    s->friction.store(normNumber(friction), std::memory_order_relaxed);
    s->restitution.store(normNumber(restitution), std::memory_order_relaxed);
    s->tag.store(tag, std::memory_order_relaxed);
    if (known == g_names.end()) {
        g_names.emplace(id, name);
        g_order.push_back(id);
    }
    return surfOk(id);
}

int32_t aver_phys_surface_tune(int32_t surface, float friction, float restitution, int32_t tag) {
    if (!std::isfinite(friction) || !std::isfinite(restitution)) return surfFail(AbiError::InvalidArgument);
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    const auto it = g_surfaces.find(surface);
    if (surface == 0 || it == g_surfaces.end()) return surfFail(AbiError::BadHandle);
    AverSurface* s = it->second.GetPtr();
    s->friction.store(normNumber(friction), std::memory_order_relaxed);
    s->restitution.store(normNumber(restitution), std::memory_order_relaxed);
    s->tag.store(tag, std::memory_order_relaxed);
    return surfOk(1);
}

int32_t aver_phys_surface_info(int32_t surface, float* outFriction, float* outRestitution, int32_t* outTag,
                               uint8_t* outName, int32_t nameCap) {
    if (nameCap < 0 || (nameCap > 0 && !outName)) return surfFail(AbiError::InvalidArgument);
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    const auto it = g_surfaces.find(surface);
    if (surface == 0 || it == g_surfaces.end()) return surfFail(AbiError::BadHandle);
    const AverSurface* s = it->second.GetPtr();
    if (outFriction) *outFriction = s->friction.load(std::memory_order_relaxed);
    if (outRestitution) *outRestitution = s->restitution.load(std::memory_order_relaxed);
    if (outTag) *outTag = s->tag.load(std::memory_order_relaxed);
    if (outName && nameCap > 0) {
        const auto n = g_names.find(surface);
        const size_t len = n == g_names.end() ? 0 : std::min(n->second.size(), static_cast<size_t>(nameCap - 1));
        if (len) std::memcpy(outName, n->second.data(), len);
        outName[len] = 0;
    }
    return surfOk(1);
}

int32_t aver_phys_surface_count(void) {
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    return static_cast<int32_t>(g_order.size());
}

int32_t aver_phys_surface_at(int32_t index) {
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    if (index < 0 || static_cast<size_t>(index) >= g_order.size()) return 0;
    return g_order[static_cast<size_t>(index)];
}

void aver_phys_surface_reset(void) {
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    for (auto& [id, s] : g_surfaces) {
        s->friction.store(-1.0f, std::memory_order_relaxed);
        s->restitution.store(-1.0f, std::memory_order_relaxed);
        s->tag.store(0, std::memory_order_relaxed);
    }
    g_names.clear();
    g_order.clear();
}

// ---- the body label ----------------------------------------------------------------------------------------------

int32_t aver_phys_body_set_surface(int32_t body, int32_t surface, int32_t copyPhysics) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    const AverSurface* s = nullptr;
    if (surface != 0) {
        surfaceMaterial(surface);   // intern, so the hook and the queries know the id from now on
        s = surfaceObject(surface);
    }
    if (const JPH::BodyID* id = findBody(body)) {
        bi().SetUserData(*id, withLabelHalf(bi().GetUserData(*id), surface));
        if (copyPhysics && s) {
            const float f = s->friction.load(std::memory_order_relaxed), r = s->restitution.load(std::memory_order_relaxed);
            if (f >= 0.0f) bi().SetFriction(*id, f);
            if (r >= 0.0f) bi().SetRestitution(*id, r);
        }
        return surfOk(1);
    }
    if (JPH::CharacterVirtual* ch = findCharacter(body)) {
        ch->SetUserData(withLabelHalf(ch->GetUserData(), surface));
        return surfOk(1);
    }
    return 0;   // findBody recorded BadHandle
}

int32_t aver_phys_body_surface(int32_t body) {
    if (!g_world) { aver::setAbiError(AbiError::NotInitialised); return -1; }
    if (const JPH::BodyID* id = findBody(body)) return bodyLabelOf(bi().GetUserData(*id));
    if (const JPH::CharacterVirtual* ch = findCharacter(body)) { aver::setAbiError(AbiError::Ok); return bodyLabelOf(ch->GetUserData()); }
    return -1;
}

// ---- the combine rule ----------------------------------------------------------------------------------------------

int32_t aver_phys_set_combine_mode(int32_t mode) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    if (mode != AVER_PHYS_COMBINE_JOLT && mode != AVER_PHYS_COMBINE_MATERIAL) return surfFail(AbiError::InvalidArgument);
    if (g_world->inStep) return surfFail(AbiError::Unsupported);   // workers read it
    g_world->combineMode = mode;
    return surfOk(1);
}

int32_t aver_phys_combine_mode(void) {
    if (!g_world) { aver::setAbiError(AbiError::NotInitialised); return -1; }
    aver::setAbiError(AbiError::Ok);
    return g_world->combineMode;
}

int32_t aver_phys_body_set_combine_flags(int32_t body, int32_t flags) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    if (flags & ~(AVER_PHYS_BODY_COMBINE_ROUGH | AVER_PHYS_BODY_COMBINE_ABSORBENT)) return surfFail(AbiError::InvalidArgument);
    if (g_world->inStep) return surfFail(AbiError::Unsupported);
    if (!findBody(body)) return 0;   // BadHandle recorded
    if (flags == 0) g_world->combineFlags.erase(body);
    else g_world->combineFlags[body] = static_cast<uint8_t>(flags);
    return surfOk(1);
}

int32_t aver_phys_body_combine_flags(int32_t body) {
    if (!g_world) { aver::setAbiError(AbiError::NotInitialised); return -1; }
    if (!findBody(body)) return -1;
    aver::setAbiError(AbiError::Ok);
    const auto it = g_world->combineFlags.find(body);
    return it == g_world->combineFlags.end() ? 0 : it->second;
}

// ---- what was hit ------------------------------------------------------------------------------------------------

int32_t aver_phys_raycast_detail(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm,
                                 uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                 float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    if (!blocksOk(outF, nF, outI, nI)) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 1e-6f) || !(maxDistCm > 0.0f)) return surfOk(0);

    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d = toJoltUnit(Vec3(dir.x / len, dir.y / len, dir.z / len)) * cmToM(maxDistCm);
    JPH::RRayCast ray{from, d};
    JPH::RayCastResult hit;
    const MaskLayerFilter layers(layerMask);
    const QueryBodyFilter skip(ignoredIdOf(ignoreBody), JPH::BodyID(), (flags & AVER_PHYS_QUERY_INCLUDE_SENSORS) != 0);
    if (!g_world->system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, layers, skip)) return surfOk(0);

    const int32_t handle = handleForBodyId(hit.mBodyID);
    if (handle == 0) return surfOk(0);
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), hit.mBodyID);
    if (!lock.Succeeded()) return surfOk(0);
    const JPH::Body& b = lock.GetBody();
    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);

    HitBlocks h;
    fillHit(h, b, handle, hit.mSubShapeID2);
    setPoint(h, fromJolt(point), fromJoltUnit(b.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point)), hit.mFraction * maxDistCm);
    writeBlocks(h.f, h.i, outF, nF, outI, nI);
    return surfOk(handle);
}

int32_t aver_phys_subshape_info(int32_t body, int32_t subShape, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    if (!blocksOk(outF, nF, outI, nI)) return 0;
    const JPH::BodyID id = bodyIdOfHandle(body);
    if (id.IsInvalid()) return surfFail(AbiError::BadHandle);
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return surfFail(AbiError::BadHandle);
    JPH::SubShapeID sub;
    sub.SetValue(static_cast<uint32_t>(subShape));
    HitBlocks h;
    fillHit(h, lock.GetBody(), body, sub);
    writeBlocks(h.f, h.i, outF, nF, outI, nI);
    return surfOk(1);
}

int32_t aver_phys_contact_detail(int32_t index, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    if (!blocksOk(outF, nF, outI, nI)) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    if (index < 0 || index >= static_cast<int32_t>(g_world->contacts.size())) return surfFail(AbiError::OutOfRange);
    const ContactEvent& e = g_world->contacts[static_cast<size_t>(index)];
    float f[AVER_PHYS_CONF_COUNT];
    int32_t iv[AVER_PHYS_CONI_COUNT];
    f[AVER_PHYS_CONF_CLOSING_SPEED_CMS] = e.closing;
    f[AVER_PHYS_CONF_FRICTION] = e.friction;
    f[AVER_PHYS_CONF_RESTITUTION] = e.restitution;
    iv[AVER_PHYS_CONI_SUB_SHAPE_A] = e.subA;
    iv[AVER_PHYS_CONI_SUB_SHAPE_B] = e.subB;
    iv[AVER_PHYS_CONI_SURFACE_A] = e.surfA;
    iv[AVER_PHYS_CONI_SURFACE_B] = e.surfB;
    writeBlocks(f, iv, outF, nF, outI, nI);
    return surfOk(1);
}

int32_t aver_phys_character_ground_detail(int32_t ch, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    if (!g_world) return surfFail(AbiError::NotInitialised);
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return surfFail(AbiError::BadHandle);
    if (!blocksOk(outF, nF, outI, nI)) return 0;
    const JPH::BodyID groundId = c->GetGroundBodyID();
    if (groundId.IsInvalid()) return surfOk(0);   // airborne, or standing on another character: no body to describe

    HitBlocks h;
    const JPH::SubShapeID sub = c->GetGroundSubShapeID();
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), groundId);
    if (lock.Succeeded()) {
        fillHit(h, lock.GetBody(), handleForBodyId(groundId), sub);
    } else {
        // The ground body went away since the last update: what Jolt remembered still names the surface.
        h.i[AVER_PHYS_HITI_SUB_SHAPE] = static_cast<int32_t>(sub.GetValue());
        h.i[AVER_PHYS_HITI_ENTITY] = static_cast<int32_t>(c->GetGroundUserData());
        const AverSurface* obj = nullptr;
        if (const JPH::PhysicsMaterial* m = c->GetGroundMaterial(); m && m->GetDebugName() == AverSurface::kName)
            obj = static_cast<const AverSurface*>(m);
        h.i[AVER_PHYS_HITI_SURFACE] = obj ? obj->id : bodyLabelOf(c->GetGroundUserData());
        if (!obj) obj = surfaceObject(h.i[AVER_PHYS_HITI_SURFACE]);
        h.f[AVER_PHYS_HITF_FRICTION] = obj ? obj->friction.load(std::memory_order_relaxed) : -1.0f;
        h.f[AVER_PHYS_HITF_RESTITUTION] = obj ? obj->restitution.load(std::memory_order_relaxed) : -1.0f;
        h.i[AVER_PHYS_HITI_TAG] = obj ? obj->tag.load(std::memory_order_relaxed) : 0;
        h.i[AVER_PHYS_HITI_CHILD] = h.i[AVER_PHYS_HITI_TRIANGLE] = h.i[AVER_PHYS_HITI_CELL_X] = h.i[AVER_PHYS_HITI_CELL_Y] = -1;
    }
    setPoint(h, fromJolt(c->GetGroundPosition()), fromJoltUnit(c->GetGroundNormal()), 0.0f);
    writeBlocks(h.f, h.i, outF, nF, outI, nI);
    return surfOk(1);
}

int32_t aver_phys_vehicle_wheel_surface(int32_t v, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    if (!blocksOk(outF, nF, outI, nI)) return 0;
    const JPH::Wheels& wheels = e->constraint->GetWheels();
    if (wheel < 0 || wheel >= static_cast<int32_t>(wheels.size())) return surfFail(AbiError::OutOfRange);
    const JPH::Wheel* w = wheels[static_cast<size_t>(wheel)];
    if (!w->HasContact()) return surfOk(0);

    HitBlocks h;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), w->GetContactBodyID());
    if (!lock.Succeeded()) return surfOk(0);
    fillHit(h, lock.GetBody(), handleForBodyId(w->GetContactBodyID()), w->GetContactSubShapeID());
    setPoint(h, fromJolt(w->GetContactPosition()), fromJoltUnit(w->GetContactNormal()), 0.0f);
    writeBlocks(h.f, h.i, outF, nF, outI, nI);
    return surfOk(1);
}

} // extern "C"
