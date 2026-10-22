// PhysicsDebug.cpp -- physics_debug_abi.h: Jolt's debug renderer as a line feed in the engine's frame (wave 11, WORLD_STATE_DEBUG
// phase C). The sink (PhysicsDebugRenderer.hpp) collects; this file decides WHAT to ask Jolt to draw, in what order and within
// what budget. Without JPH_DEBUG_RENDERER (cmake AVER_PHYSICS_DEBUG_RENDERER=OFF) only the "not available" answers are built.
#include "PhysicsDebugRenderer.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_debug_abi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>

#include <array>
#include <map>
#include <unordered_set>
#endif

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

std::atomic<int32_t> g_scheme{AVER_PHYS_DBG_SCHEME_MOTION};
std::atomic<uint32_t> g_armedMirror{0};   // lets debugStepBegin skip without touching the sink

constexpr uint32_t kArmBits = AVER_PHYS_DBG_CONTACTS | AVER_PHYS_DBG_SUPPORT_FACES | AVER_PHYS_DBG_LINEAR_CAST;

} // namespace

#ifdef JPH_DEBUG_RENDERER

// ---- the sink -----------------------------------------------------------------------------------------------------

namespace aver::physics::detail {

LineSinkRenderer& debugSink() {
    // Jolt allows one DebugRenderer::sInstance and the allocator must be registered first (aver_phys_init), so this is
    // reached only with a world. Never destroyed: it lives as long as the process, like Jolt's own factory.
    static LineSinkRenderer* sink = new LineSinkRenderer;
    return *sink;
}

void LineSinkRenderer::beginCollect(const float* camCm, float radiusCm, int maxLines) {
    std::lock_guard<std::mutex> lk(mutex_);
    lines_.clear();
    labels_.clear();
    maxLines_ = maxLines < 0 ? 0 : maxLines;
    truncated_ = false;
    have_camera_ = camCm != nullptr;
    if (camCm) {
        cam_cm_[0] = camCm[0]; cam_cm_[1] = camCm[1]; cam_cm_[2] = camCm[2];
        cam_jolt_ = toJolt(Vec3(camCm[0], camCm[1], camCm[2]));
    }
    radius_cm_ = (camCm && radiusCm > 0.0f) ? radiusCm : 0.0f;
}

bool LineSinkRenderer::farFromCamera(const float p[3]) const {
    if (radius_cm_ <= 0.0f) return false;
    const float dx = p[0] - cam_cm_[0], dy = p[1] - cam_cm_[1], dz = p[2] - cam_cm_[2];
    return dx * dx + dy * dy + dz * dz > radius_cm_ * radius_cm_;
}

void LineSinkRenderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) {
    const Vec3 a = fromJolt(JPH::Vec3(inFrom));
    const Vec3 b = fromJolt(JPH::Vec3(inTo));
    const float r = static_cast<float>(inColor.r) / 255.0f, g = static_cast<float>(inColor.g) / 255.0f,
                bl = static_cast<float>(inColor.b) / 255.0f;
    const DebugLine l = {{a.x, a.y, a.z}, {r, g, bl}, {b.x, b.y, b.z}, {r, g, bl}};
    const bool inStep = in_step_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mutex_);
    if (inStep) {                       // a worker thread inside Update: the armed capture, culled when it is read
        if (step_lines_.size() < kArmedCap) step_lines_.push_back(l);
        return;
    }
    if (farFromCamera(l.a) && farFromCamera(l.b)) return;
    if (static_cast<int>(lines_.size()) >= maxLines_) { truncated_ = true; return; }
    lines_.push_back(l);
}

void LineSinkRenderer::DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float) {
    if (in_step_.load(std::memory_order_relaxed)) return;
    const Vec3 p = fromJolt(JPH::Vec3(inPosition));
    DebugLabelRec rec;
    rec.pos[0] = p.x; rec.pos[1] = p.y; rec.pos[2] = p.z;
    rec.rgb[0] = static_cast<float>(inColor.r) / 255.0f;
    rec.rgb[1] = static_cast<float>(inColor.g) / 255.0f;
    rec.rgb[2] = static_cast<float>(inColor.b) / 255.0f;
    rec.text.assign(inString.data(), inString.size());
    std::lock_guard<std::mutex> lk(mutex_);
    if (labels_.size() >= kLabelCap || farFromCamera(rec.pos)) return;
    labels_.push_back(std::move(rec));
}

void LineSinkRenderer::appendArmed() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const DebugLine& l : step_lines_) {
        if (farFromCamera(l.a) && farFromCamera(l.b)) continue;
        if (static_cast<int>(lines_.size()) >= maxLines_) { truncated_ = true; break; }
        lines_.push_back(l);
    }
}

void LineSinkRenderer::setArmed(uint32_t mask) {
    mask &= kArmBits;
    armed_mask_.store(mask, std::memory_order_relaxed);
    JPH::ContactConstraintManager::sDrawContactPoint = (mask & AVER_PHYS_DBG_CONTACTS) != 0;
    JPH::ContactConstraintManager::sDrawContactManifolds = (mask & AVER_PHYS_DBG_CONTACTS) != 0;
    JPH::ContactConstraintManager::sDrawSupportingFaces = (mask & AVER_PHYS_DBG_SUPPORT_FACES) != 0;
    JPH::PhysicsSystem::sDrawMotionQualityLinearCast = (mask & AVER_PHYS_DBG_LINEAR_CAST) != 0;
    if (mask == 0) {
        in_step_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mutex_);
        step_lines_.clear();
    }
}

void LineSinkRenderer::stepBegin() {
    if (armed() == 0) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        step_lines_.clear();
    }
    in_step_.store(true, std::memory_order_relaxed);
}

// A batch is reduced to its feature edges once, here. Vertices are merged by position (to 1e-4 of the model's unit), an edge's two
// adjacent triangles are compared by normal, and an edge between triangles within ~1.8 degrees of each other is a tessellation
// diagonal and is not kept. A boundary edge, or one shared by more than two triangles, always is.
JPH::DebugRenderer::Batch LineSinkRenderer::makeBatch(const std::vector<Vertex>& v) {
    EdgeBatch* batch = new EdgeBatch;
    Batch result = batch;

    std::map<std::array<int32_t, 3>, JPH::uint32> vertIndex;
    auto indexOf = [&](const JPH::Float3& p) -> JPH::uint32 {
        const std::array<int32_t, 3> key = {static_cast<int32_t>(std::lround(p.x * 10000.0f)),
                                            static_cast<int32_t>(std::lround(p.y * 10000.0f)),
                                            static_cast<int32_t>(std::lround(p.z * 10000.0f))};
        const auto it = vertIndex.find(key);
        if (it != vertIndex.end()) return it->second;
        const JPH::uint32 idx = static_cast<JPH::uint32>(batch->verts.size());
        batch->verts.push_back(p);
        vertIndex.emplace(key, idx);
        return idx;
    };

    struct Acc {
        int count = 0;
        JPH::Vec3 n[2] = {JPH::Vec3::sZero(), JPH::Vec3::sZero()};
        JPH::Color color = JPH::Color::sWhite;
    };
    std::map<std::pair<JPH::uint32, JPH::uint32>, Acc> edges;

    for (size_t t = 0; t + 2 < v.size(); t += 3) {
        const JPH::uint32 idx[3] = {indexOf(v[t].mPosition), indexOf(v[t + 1].mPosition), indexOf(v[t + 2].mPosition)};
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) continue;
        const JPH::Vec3 p0(v[t].mPosition), p1(v[t + 1].mPosition), p2(v[t + 2].mPosition);
        JPH::Vec3 n = (p1 - p0).Cross(p2 - p0);
        const float len = n.Length();
        if (len < 1.0e-12f) continue;
        n = n / len;
        for (int e = 0; e < 3; ++e) {
            JPH::uint32 a = idx[e], b = idx[(e + 1) % 3];
            if (a > b) std::swap(a, b);
            Acc& acc = edges[{a, b}];
            if (acc.count == 0) acc.color = v[t].mColor;
            if (acc.count < 2) acc.n[acc.count] = n;
            ++acc.count;
        }
    }
    for (const auto& [key, acc] : edges) {
        const bool keep = acc.count == 1 || acc.count > 2 || acc.n[0].Dot(acc.n[1]) < 0.9995f;
        if (keep) batch->edges.push_back({key.first, key.second, acc.color});
    }
    return result;
}

JPH::DebugRenderer::Batch LineSinkRenderer::CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) {
    std::vector<Vertex> v;
    if (inTriangles && inTriangleCount > 0) {
        v.reserve(static_cast<size_t>(inTriangleCount) * 3);
        for (int t = 0; t < inTriangleCount; ++t)
            for (int k = 0; k < 3; ++k) v.push_back(inTriangles[t].mV[k]);
    }
    return makeBatch(v);
}

JPH::DebugRenderer::Batch LineSinkRenderer::CreateTriangleBatch(const Vertex* inVertices, int inVertexCount, const JPH::uint32* inIndices, int inIndexCount) {
    std::vector<Vertex> v;
    if (inVertices && inVertexCount > 0 && inIndices && inIndexCount > 0) {
        v.reserve(static_cast<size_t>(inIndexCount));
        for (int i = 0; i + 2 < inIndexCount; i += 3)
            for (int k = 0; k < 3; ++k) {
                const JPH::uint32 ix = inIndices[i + k];
                if (ix < static_cast<JPH::uint32>(inVertexCount)) v.push_back(inVertices[ix]);
                else v.push_back(inVertices[0]);
            }
    }
    return makeBatch(v);
}

void LineSinkRenderer::DrawGeometry(JPH::RMat44Arg inModelMatrix, const JPH::AABox& inWorldSpaceBounds, float inLODScaleSq, JPH::ColorArg inModelColor,
                                    const GeometryRef& inGeometry, ECullMode, ECastShadow, EDrawMode) {
    if (!inGeometry || inGeometry->mLODs.empty()) return;
    const LOD* lod = inGeometry->mLODs.data();
    if (have_camera_) lod = &inGeometry->GetLOD(cam_jolt_, inWorldSpaceBounds, inLODScaleSq);
    const EdgeBatch* batch = static_cast<const EdgeBatch*>(lod->mTriangleBatch.GetPtr());
    if (!batch) return;
    for (const EdgeBatch::Edge& e : batch->edges) {
        const JPH::RVec3 p0 = inModelMatrix * JPH::Vec3(batch->verts[e.a]);
        const JPH::RVec3 p1 = inModelMatrix * JPH::Vec3(batch->verts[e.b]);
        DrawLine(p0, p1, inModelColor * e.color);
    }
}

} // namespace aver::physics::detail

// ---- what a collect draws -----------------------------------------------------------------------------------------

namespace {

// A mesh or heightfield, however wrapped: those are drawn as the triangles near the camera, never whole.
bool isHeavy(const JPH::Shape* s, int depth = 0) {
    if (!s || depth > 8) return false;
    switch (s->GetSubType()) {
        case JPH::EShapeSubType::Mesh:
        case JPH::EShapeSubType::HeightField:
            return true;
        case JPH::EShapeSubType::RotatedTranslated:
        case JPH::EShapeSubType::Scaled:
        case JPH::EShapeSubType::OffsetCenterOfMass:
            return isHeavy(static_cast<const JPH::DecoratedShape*>(s)->GetInnerShape(), depth + 1);
        case JPH::EShapeSubType::StaticCompound:
        case JPH::EShapeSubType::MutableCompound: {
            const JPH::CompoundShape* c = static_cast<const JPH::CompoundShape*>(s);
            for (JPH::uint i = 0; i < c->GetNumSubShapes(); ++i)
                if (isHeavy(c->GetSubShape(i).mShape.GetPtr(), depth + 1)) return true;
            return false;
        }
        default:
            return false;
    }
}

// Which bodies a DrawBodies pass takes: within the radius, and either the light ones or the heavy ones.
class RadiusFilter final : public JPH::BodyDrawFilter {
public:
    enum class Take { Light, Heavy };
    RadiusFilter(Take take, bool cull, JPH::Vec3Arg cam, float radiusM) : take_(take), cull_(cull), cam_(cam), r2_(radiusM * radiusM) {}
    bool ShouldDraw(const JPH::Body& body) const override {
        const bool heavy = isHeavy(body.GetShape());
        if (heavy != (take_ == Take::Heavy)) return false;
        if (!cull_) return true;
        return body.GetWorldSpaceBounds().GetSqDistanceTo(cam_) <= r2_;
    }
private:
    Take take_;
    bool cull_;
    JPH::Vec3 cam_;
    float r2_;
};

JPH::BodyManager::EShapeColor shapeColor(int32_t scheme, uint32_t flags) {
    switch (scheme) {
        case AVER_PHYS_DBG_SCHEME_SLEEP:    return JPH::BodyManager::EShapeColor::SleepColor;
        case AVER_PHYS_DBG_SCHEME_SHAPE:    return JPH::BodyManager::EShapeColor::ShapeTypeColor;
        case AVER_PHYS_DBG_SCHEME_ISLAND:   return JPH::BodyManager::EShapeColor::IslandColor;
        case AVER_PHYS_DBG_SCHEME_INSTANCE: return JPH::BodyManager::EShapeColor::InstanceColor;
        default: break;
    }
    // With the sleep overlay on, the default scheme shows who is asleep.
    return (flags & AVER_PHYS_DBG_SLEEP) ? JPH::BodyManager::EShapeColor::SleepColor : JPH::BodyManager::EShapeColor::MotionTypeColor;
}

void drawBodies(World& w, LineSinkRenderer& sink, uint32_t flags, int32_t scheme, bool cull, JPH::Vec3Arg cam, float radiusM) {
    constexpr uint32_t kBodyFlags = AVER_PHYS_DBG_BODIES | AVER_PHYS_DBG_BOUNDS | AVER_PHYS_DBG_COM | AVER_PHYS_DBG_VELOCITY |
                                    AVER_PHYS_DBG_SLEEP | AVER_PHYS_DBG_MASS | AVER_PHYS_DBG_SOFT_VERTICES | AVER_PHYS_DBG_SOFT_EDGES;
    if (!(flags & kBodyFlags)) return;

    JPH::BodyManager::DrawSettings ds;
    ds.mDrawShapeColor = shapeColor(scheme, flags);
    ds.mDrawShape = (flags & AVER_PHYS_DBG_BODIES) != 0 && scheme != AVER_PHYS_DBG_SCHEME_LAYER;
    ds.mDrawShapeWireframe = true;
    ds.mDrawBoundingBox = (flags & AVER_PHYS_DBG_BOUNDS) != 0;
    ds.mDrawCenterOfMassTransform = (flags & AVER_PHYS_DBG_COM) != 0;
    ds.mDrawVelocity = (flags & AVER_PHYS_DBG_VELOCITY) != 0;
    ds.mDrawSleepStats = (flags & AVER_PHYS_DBG_SLEEP) != 0;
    ds.mDrawMassAndInertia = (flags & AVER_PHYS_DBG_MASS) != 0;
    ds.mDrawSoftBodyVertices = (flags & AVER_PHYS_DBG_SOFT_VERTICES) != 0;
    const bool softEdges = (flags & AVER_PHYS_DBG_SOFT_EDGES) != 0;
    ds.mDrawSoftBodyEdgeConstraints = softEdges;
    ds.mDrawSoftBodyBendConstraints = softEdges;
    ds.mDrawSoftBodyVolumeConstraints = softEdges;
    ds.mDrawSoftBodySkinConstraints = softEdges;
    ds.mDrawSoftBodyLRAConstraints = softEdges;

    const RadiusFilter light(RadiusFilter::Take::Light, cull, cam, radiusM);
    w.system.DrawBodies(ds, &sink, &light);

    // Heavy bodies get everything but the shape, which the static-triangle pass owns.
    constexpr uint32_t kHeavyFlags = AVER_PHYS_DBG_BOUNDS | AVER_PHYS_DBG_COM | AVER_PHYS_DBG_VELOCITY | AVER_PHYS_DBG_SLEEP | AVER_PHYS_DBG_MASS;
    if ((flags & kHeavyFlags) && !sink.full()) {
        ds.mDrawShape = false;
        ds.mDrawSoftBodyVertices = ds.mDrawSoftBodyEdgeConstraints = ds.mDrawSoftBodyBendConstraints = false;
        ds.mDrawSoftBodyVolumeConstraints = ds.mDrawSoftBodySkinConstraints = ds.mDrawSoftBodyLRAConstraints = false;
        const RadiusFilter heavy(RadiusFilter::Take::Heavy, cull, cam, radiusM);
        w.system.DrawBodies(ds, &sink, &heavy);
    }
}

// The collision-layer scheme has no Jolt equivalent, so the shapes are drawn here, one colour per user layer.
void drawBodiesByLayer(World& w, LineSinkRenderer& sink, bool cull, JPH::Vec3Arg cam, float radiusM) {
    const JPH::BodyLockInterface& iface = w.system.GetBodyLockInterfaceNoLock();
    const float r2 = radiusM * radiusM;
    for (const auto& [handle, id] : w.bodies) {
        if (sink.full()) return;
        JPH::BodyLockRead lock(iface, id);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        if (!body.IsInBroadPhase() || isHeavy(body.GetShape())) continue;
        if (cull && body.GetWorldSpaceBounds().GetSqDistanceTo(cam) > r2) continue;
        const JPH::Color color = body.IsSensor() ? JPH::Color::sYellow
                                                 : JPH::Color::sGetDistinctColor(static_cast<int>(Layers::userOf(body.GetObjectLayer())) + 1);
        body.GetShape()->Draw(&sink, body.GetCenterOfMassTransform(), JPH::Vec3::sOne(), color, false, true);
    }
}

void drawCharacters(World& w, LineSinkRenderer& sink, bool cull, JPH::Vec3Arg cam, float radiusM) {
    const float r2 = radiusM * radiusM;
    for (const auto& [handle, ch] : w.characters) {
        if (sink.full()) return;
        if (cull && (JPH::Vec3(ch->GetPosition()) - cam).LengthSq() > r2) continue;
        ch->GetShape()->Draw(&sink, ch->GetCenterOfMassTransform(), JPH::Vec3::sOne(), JPH::Color::sYellow, false, true);
        if (ch->GetGroundState() != JPH::CharacterBase::EGroundState::InAir) {
            const JPH::RVec3 g = ch->GetGroundPosition();
            sink.DrawArrow(g, g + ch->GetGroundNormal() * 0.5f, JPH::Color::sGreen, 0.05f);
        }
    }
}

// Every mesh or heightfield body that reaches into the camera sphere, as the triangles inside it. Each edge once per body.
void drawStaticTriangles(World& w, LineSinkRenderer& sink, JPH::Vec3Arg cam, float radiusM) {
    constexpr int kBatch = 96;   // at least Shape::cGetTrianglesMinTrianglesRequested (32)
    const JPH::BodyLockInterface& iface = w.system.GetBodyLockInterfaceNoLock();
    const float r2 = radiusM * radiusM;
    const JPH::AABox box(cam, radiusM);
    JPH::Float3 tri[3 * kBatch];
    for (const auto& [handle, id] : w.bodies) {
        if (sink.full()) return;
        JPH::BodyLockRead lock(iface, id);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        if (!body.IsInBroadPhase() || !isHeavy(body.GetShape())) continue;
        if (body.GetWorldSpaceBounds().GetSqDistanceTo(cam) > r2) continue;

        const bool field = body.GetShape()->GetSubType() == JPH::EShapeSubType::HeightField;
        const JPH::Color color = field ? JPH::Color(120, 205, 120) : JPH::Color(90, 140, 255);
        const JPH::Shape* shape = body.GetShape();
        alignas(16) JPH::Shape::GetTrianglesContext ctx;
        shape->GetTrianglesStart(ctx, box, JPH::Vec3(body.GetCenterOfMassPosition()), body.GetRotation(), JPH::Vec3::sOne());
        std::unordered_set<uint64_t> seen;
        auto vkey = [](const JPH::Float3& p) {
            uint32_t x, y, z;
            std::memcpy(&x, &p.x, 4); std::memcpy(&y, &p.y, 4); std::memcpy(&z, &p.z, 4);
            return (static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full) ^
                   (static_cast<uint64_t>(z) * 0x165667B19E3779F9ull);
        };
        for (;;) {
            const int n = shape->GetTrianglesNext(ctx, kBatch, tri);
            if (n <= 0) break;
            for (int t = 0; t < n; ++t) {
                const JPH::Float3* v = tri + 3 * t;
                // The shape culls coarsely; keep a triangle only if a corner is inside the sphere.
                bool inside = false;
                for (int k = 0; k < 3 && !inside; ++k) inside = (JPH::Vec3(v[k]) - cam).LengthSq() <= r2;
                if (!inside) continue;
                for (int e = 0; e < 3; ++e) {
                    const JPH::Float3& a = v[e];
                    const JPH::Float3& b = v[(e + 1) % 3];
                    const uint64_t ka = vkey(a), kb = vkey(b);
                    const uint64_t key = ka < kb ? ka * 31u + kb : kb * 31u + ka;
                    if (!seen.insert(key).second) continue;
                    sink.DrawLine(JPH::RVec3(JPH::Vec3(a)), JPH::RVec3(JPH::Vec3(b)), color);
                }
                if (sink.full()) return;
            }
        }
    }
}

void collectLabels(World& w, LineSinkRenderer& sink, bool cull, JPH::Vec3Arg cam, float radiusM) {
    const JPH::BodyLockInterface& iface = w.system.GetBodyLockInterfaceNoLock();
    const float r2 = radiusM * radiusM;
    for (const auto& [handle, id] : w.bodies) {
        JPH::BodyLockRead lock(iface, id);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        if (!body.IsInBroadPhase() || body.IsStatic() || body.IsSensor()) continue;
        const JPH::Vec3 com(body.GetCenterOfMassPosition());
        if (cull && (com - cam).LengthSq() > r2) continue;
        float massKg = 0.0f;
        if (body.IsDynamic() && body.GetMotionProperties()->GetInverseMass() > 0.0f) massKg = 1.0f / body.GetMotionProperties()->GetInverseMass();
        const float speedCmS = body.GetLinearVelocity().Length() * kCmPerMetre;
        char text[64];
        std::snprintf(text, sizeof text, "#%d  %.0f kg  %.0f cm/s", handle, static_cast<double>(massKg), static_cast<double>(speedCmS));
        sink.DrawText3D(JPH::RVec3(com + JPH::Vec3(0.0f, 0.3f, 0.0f)), text, body.IsActive() ? JPH::Color::sWhite : JPH::Color::sGrey, 0.2f);
    }
}

int collect(uint32_t flags, const float* cameraXyz, float radiusCm, int maxLines) {
    World& w = *g_world;
    LineSinkRenderer& sink = debugSink();
    sink.beginCollect(cameraXyz, radiusCm, maxLines);
    int32_t scheme = g_scheme.load(std::memory_order_relaxed);
    if (flags & AVER_PHYS_DBG_ISLANDS) { flags |= AVER_PHYS_DBG_BODIES; scheme = AVER_PHYS_DBG_SCHEME_ISLAND; }

    const bool cull = sink.cameraSet() && sink.radiusMetres() > 0.0f;
    const JPH::Vec3 cam = sink.cameraSet() ? sink.cameraJolt() : JPH::Vec3::sZero();
    const float radiusM = sink.radiusMetres();

    drawBodies(w, sink, flags, scheme, cull, cam, radiusM);
    if ((flags & AVER_PHYS_DBG_BODIES) && scheme == AVER_PHYS_DBG_SCHEME_LAYER && !sink.full()) drawBodiesByLayer(w, sink, cull, cam, radiusM);
    if ((flags & (AVER_PHYS_DBG_CONSTRAINTS | AVER_PHYS_DBG_LIMITS)) && !sink.full()) {
        if (flags & AVER_PHYS_DBG_CONSTRAINTS) {
            w.system.DrawConstraints(&sink);
            w.system.DrawConstraintReferenceFrame(&sink);
        }
        if (flags & AVER_PHYS_DBG_LIMITS) w.system.DrawConstraintLimits(&sink);
    }
    if ((flags & AVER_PHYS_DBG_CHARACTERS) && !sink.full()) drawCharacters(w, sink, cull, cam, radiusM);
    // The static-triangle pass needs a camera and a radius: without them it would be the whole level.
    if ((flags & AVER_PHYS_DBG_STATIC_TRIANGLES) && cull && !sink.full()) drawStaticTriangles(w, sink, cam, radiusM);
    if ((flags & AVER_PHYS_DBG_LABELS) && !sink.full()) collectLabels(w, sink, cull, cam, radiusM);
    if ((flags & kArmBits) && sink.armed() != 0) sink.appendArmed();
    sink.NextFrame();   // lets Jolt drop the geometry it built for sizes it no longer draws
    return sink.lineCount();
}

} // namespace

namespace aver::physics::detail {

void debugStepBegin() {
    if (g_armedMirror.load(std::memory_order_relaxed) == 0) return;
    debugSink().stepBegin();
}

void debugStepEnd() {
    if (g_armedMirror.load(std::memory_order_relaxed) == 0) return;
    debugSink().stepEnd();
}

} // namespace aver::physics::detail

#else // no debug renderer in this build

namespace aver::physics::detail {
void debugStepBegin() {}
void debugStepEnd() {}
}

#endif

// ---- ABI ----------------------------------------------------------------------------------------------------------

extern "C" {

int32_t aver_phys_debug_abi_version(void) { return AVER_PHYS_DEBUG_ABI_VERSION; }

int32_t aver_phys_debug_available(void) {
#ifdef JPH_DEBUG_RENDERER
    return 1;
#else
    return 0;
#endif
}

int32_t aver_phys_debug_set_color_scheme(int32_t scheme) {
    if (scheme < 0 || scheme >= AVER_PHYS_DBG_SCHEME_COUNT) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    g_scheme.store(scheme, std::memory_order_relaxed);
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_debug_collect(uint32_t flags, const float* cameraXyz, float radiusCm, int32_t maxLines) {
#ifdef JPH_DEBUG_RENDERER
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return 0; }
    if (flags == 0 || maxLines <= 0) {
        debugSink().beginCollect(nullptr, 0.0f, 0);   // an empty answer replaces the last one
        return 0;
    }
    aver::setAbiError(aver::AbiError::Ok);
    return collect(flags, cameraXyz, radiusCm, maxLines);
#else
    (void)flags; (void)cameraXyz; (void)radiusCm; (void)maxLines;
    aver::setAbiError(aver::AbiError::Unsupported);
    return 0;
#endif
}

int32_t aver_phys_debug_truncated(void) {
#ifdef JPH_DEBUG_RENDERER
    return g_world && debugSink().truncated() ? 1 : 0;
#else
    return 0;
#endif
}

int32_t aver_phys_debug_lines(float* outLines, int32_t maxLines) {
#ifdef JPH_DEBUG_RENDERER
    if (!outLines || maxLines <= 0 || !g_world) return 0;
    const std::vector<DebugLine>& l = debugSink().lines();
    const int32_t n = std::min(maxLines, static_cast<int32_t>(l.size()));
    if (n > 0) std::memcpy(outLines, l.data(), static_cast<size_t>(n) * sizeof(DebugLine));
    return n;
#else
    (void)outLines; (void)maxLines;
    return 0;
#endif
}

int32_t aver_phys_debug_label_count(void) {
#ifdef JPH_DEBUG_RENDERER
    return g_world ? static_cast<int32_t>(debugSink().labels().size()) : 0;
#else
    return 0;
#endif
}

int32_t aver_phys_debug_label_get(int32_t index, float* outXyz, float* outRgb, uint8_t* outTextUtf8, int32_t textCap) {
#ifdef JPH_DEBUG_RENDERER
    if (!g_world) return 0;
    const std::vector<DebugLabelRec>& labels = debugSink().labels();
    if (index < 0 || index >= static_cast<int32_t>(labels.size())) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    const DebugLabelRec& r = labels[static_cast<size_t>(index)];
    if (outXyz) std::memcpy(outXyz, r.pos, sizeof r.pos);
    if (outRgb) std::memcpy(outRgb, r.rgb, sizeof r.rgb);
    if (outTextUtf8 && textCap > 0) {
        const size_t n = std::min(r.text.size(), static_cast<size_t>(textCap - 1));
        std::memcpy(outTextUtf8, r.text.data(), n);
        outTextUtf8[n] = 0;
    }
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
#else
    (void)index; (void)outXyz; (void)outRgb; (void)outTextUtf8; (void)textCap;
    return 0;
#endif
}

int32_t aver_phys_debug_arm(uint32_t flags) {
#ifdef JPH_DEBUG_RENDERER
    if (!g_world) return 0;   // the sink needs Jolt's allocator, which exists with the first world
    const uint32_t mask = flags & kArmBits;
    if (mask == 0 && g_armedMirror.load(std::memory_order_relaxed) == 0) return 0;   // nothing armed, nothing to disarm
    debugSink().setArmed(mask);
    g_armedMirror.store(mask, std::memory_order_relaxed);
    return static_cast<int32_t>(mask);
#else
    (void)flags;
    return 0;
#endif
}

} // extern "C"
