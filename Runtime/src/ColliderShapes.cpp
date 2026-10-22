// ColliderShapes: an .ocshape tree -> physics shape handles, the host seam, and the CollisionShape component. See ColliderShapes.hpp.
#include "aver/game/ColliderShapes.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/formats/OcSurf.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <utility>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_shape_builder_abi.h"
#  include "aver/physics/physics_surface_abi.h"
#endif

#if AVER_MODULE_SCENE
#  include "aver/core/Assert.hpp"
#  include "aver/core/CharConv.hpp"
#  include "aver/scene/Components.hpp"
#endif

namespace aver::game {

// ---- the host ------------------------------------------------------------------------------------------------------------------

namespace {
ColliderShapeHost& hostSlot() {
    static ColliderShapeHost h;
    return h;
}

std::string slashed(std::string_view p) {
    std::string o(p);
    std::replace(o.begin(), o.end(), '\\', '/');
    return o;
}
} // namespace

void setColliderShapeHost(ColliderShapeHost host) { hostSlot() = std::move(host); }
const ColliderShapeHost& colliderShapeHost() { return hostSlot(); }

ColliderShapeHost makeColliderShapeHost(GameContent& content, std::string contentDir, std::function<std::string(u64)> pathOfId) {
    ColliderShapeHost host;
    host.readShape = [contentDir, pathOfId](u64 id, std::string& text, std::string& label) {
        if (!pathOfId || contentDir.empty()) return false;
        const std::string rel = pathOfId(id);
        if (rel.empty()) return false;
        label = rel;
        return readFileText(contentDir + AVER_PATH_SEP + rel, text);
    };
#if AVER_MODULE_SCENE
    GameContent* c = &content;
    host.meshes.mesh = [c](const std::string& contentPath, ColliderMesh& out) {
        const GameContent::CollisionMesh* cm = c->collisionMeshFor(fnv1a64(std::string_view(slashed(contentPath))));
        if (!cm || cm->positions.empty() || cm->indices.size() < 3) return false;
        out.vertices = cm->positions;
        out.indices.assign(cm->indices.begin(), cm->indices.end());
        out.triPalette = cm->triPalette;
        out.palette = cm->palette;
        return true;
    };
#else
    (void)content;
#endif
    return host;
}

// ---- the walker ----------------------------------------------------------------------------------------------------------------

bool buildColliderShape(const fmt::OcShapeData& spec, const ColliderMeshSource& meshes, i32& outShape, std::string& why) {
    outShape = 0;
#if AVER_MODULE_PHYSICS
    using fmt::ShapeNodeKind;
    const std::vector<std::string> problems = fmt::validateOcShape(spec);
    if (!problems.empty()) { why = problems.front(); return false; }
    const std::vector<u32> order = fmt::ocShapeBuildOrder(spec);
    if (order.empty()) { why = "the shape has no ROOT"; return false; }

    std::unordered_map<std::string, u32> idx;
    for (u32 i = 0; i < spec.nodes.size(); ++i) idx.emplace(spec.nodes[i].id, i);
    std::unordered_map<std::string, std::vector<const fmt::ShapeChildRec*>> kids;
    for (const fmt::ShapeChildRec& c : spec.children) kids[c.compound].push_back(&c);

    std::vector<i32> handle(spec.nodes.size(), 0);
    const auto releaseAll = [&](i32 keep) {
        for (i32& h : handle) {
            if (h != 0 && h != keep) aver_phys_shape_release(h);
            if (h != keep) h = 0;
        }
    };
    const auto refuse = [&](const fmt::ShapeNode& n, const char* what) {
        why = "node '" + n.id + "': " + what + " (physics error " + std::to_string(aver_phys_last_error()) + "; see the log)";
        releaseAll(0);
        return false;
    };

    const i32 defaultSurface = aver::surfaceIdFromName(spec.surface);
    for (const u32 at : order) {
        const fmt::ShapeNode& n = spec.nodes[at];
        const float* a = n.nums.data();
        i32 h = 0;

        if (fmt::shapeKindIsLeaf(n.kind)) {
            const i32 surface = n.surface.empty() ? defaultSurface : aver::surfaceIdFromName(n.surface);
            float p[AVER_PHYS_SP_COUNT] = {n.radius, n.density, n.tolerance, 5.0f, -1.0f, -1.0f};
            i32 pn = 3;
            if (n.kind == ShapeNodeKind::Mesh && (n.hasEdge || n.quality >= 0)) {
                if (n.hasEdge) p[AVER_PHYS_SP_EDGE_ANGLE_DEG] = n.edge;
                p[AVER_PHYS_SP_BUILD_QUALITY] = static_cast<float>(n.quality);
                pn = AVER_PHYS_SP_COUNT;
            }
            switch (n.kind) {
                case ShapeNodeKind::Box: h = aver_phys_shape_box(a[0], a[1], a[2], surface, p, pn); break;
                case ShapeNodeKind::Sphere: h = aver_phys_shape_sphere(a[0], surface, p, pn); break;
                case ShapeNodeKind::Capsule: h = aver_phys_shape_capsule(a[0], a[1], surface, p, pn); break;
                case ShapeNodeKind::Cylinder: h = aver_phys_shape_cylinder(a[0], a[1], surface, p, pn); break;
                case ShapeNodeKind::TaperedCapsule: h = aver_phys_shape_tapered_capsule(a[0], a[1], a[2], surface, p, pn); break;
                case ShapeNodeKind::TaperedCylinder: h = aver_phys_shape_tapered_cylinder(a[0], a[1], a[2], surface, p, pn); break;
                case ShapeNodeKind::Triangle: h = aver_phys_shape_triangle(a, surface, p, pn); break;
                case ShapeNodeKind::Plane: h = aver_phys_shape_plane(a, a[3], a[4], surface); break;
                case ShapeNodeKind::Empty: h = aver_phys_shape_empty(a); break;
                case ShapeNodeKind::HullPoints:
                    h = aver_phys_shape_convex_hull(a, static_cast<i32>(n.nums.size() / 3), surface, p, pn);
                    break;
                case ShapeNodeKind::HullMesh:
                case ShapeNodeKind::Mesh: {
                    ColliderMesh m;
                    if (!meshes.mesh || !meshes.mesh(n.path, m) || m.vertices.size() < 9 || m.indices.size() < 3) {
                        why = "node '" + n.id + "': the mesh '" + n.path + "' is not available";
                        releaseAll(0);
                        return false;
                    }
                    if (n.kind == ShapeNodeKind::HullMesh) {
                        h = aver_phys_shape_convex_hull(m.vertices.data(), static_cast<i32>(m.vertices.size() / 3), surface, p, pn);
                        break;
                    }
                    // The slots' surfaces win; the leaf's (or the file's default) fills the slots that name none, and is the only
                    // surface of a mesh whose slots name nothing.
                    std::vector<i32> palette = m.palette;
                    if (surface != 0) {
                        if (palette.empty()) palette.push_back(surface);
                        else for (i32& s : palette) if (s == 0) s = surface;
                    }
                    const bool perTriangle = !m.palette.empty() && m.triPalette.size() * 3 >= m.indices.size();
                    h = aver_phys_shape_mesh(m.vertices.data(), static_cast<i32>(m.vertices.size() / 3), m.indices.data(),
                                             static_cast<i32>(m.indices.size()), perTriangle ? m.triPalette.data() : nullptr,
                                             palette.empty() ? nullptr : palette.data(), static_cast<i32>(palette.size()), nullptr, p, pn);
                    break;
                }
                default: break;
            }
        } else if (n.kind == ShapeNodeKind::Compound) {
            const auto& list = kids[n.id];
            const usize count = list.size();
            std::vector<i32> ch(count);
            std::vector<float> pos(3 * count), rot(4 * count);
            std::vector<i32> user(count);
            for (usize i = 0; i < count; ++i) {
                ch[i] = handle[idx[list[i]->node]];
                std::copy(list[i]->pos, list[i]->pos + 3, pos.begin() + static_cast<std::ptrdiff_t>(3 * i));
                std::copy(list[i]->rot, list[i]->rot + 4, rot.begin() + static_cast<std::ptrdiff_t>(4 * i));
                user[i] = list[i]->user;
            }
            h = aver_phys_shape_compound(n.mutableCompound ? AVER_PHYS_COMPOUND_MUTABLE : AVER_PHYS_COMPOUND_STATIC, ch.data(), pos.data(),
                                         rot.data(), user.data(), static_cast<i32>(count));
        } else {
            const i32 inner = handle[idx[n.child]];
            if (n.kind == ShapeNodeKind::RotatedTranslated) h = aver_phys_shape_rotated_translated(inner, n.pos, n.rot);
            else if (n.kind == ShapeNodeKind::OffsetCom) h = aver_phys_shape_offset_com(inner, n.pos);
            else h = aver_phys_shape_scaled(inner, n.scale);
        }
        if (h == 0) return refuse(n, "the physics module refused the shape");
        handle[at] = h;
    }

    const i32 root = handle[order.back()];
    releaseAll(root);
    outShape = root;
    return true;
#else
    (void)spec; (void)meshes;
    why = "this build has no physics module";
    return false;
#endif
}

// ---- the component -------------------------------------------------------------------------------------------------------------

#if AVER_MODULE_SCENE

namespace {

constexpr const char* kTypeName = "CCollisionShape";

bool parseNum(std::string_view s, double& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    double v = 0.0;
    const auto r = aver::fromChars(first, last, v);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    out = v;
    return true;
}

std::string hexOf(u64 v, int digits) {
    char b[24];
    std::snprintf(b, sizeof b, "%0*llx", digits, static_cast<unsigned long long>(v));
    return b;
}

bool parseHex(std::string_view s, u64& out) {
    if (s.empty() || s.size() > 16) return false;
    u64 h = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), h, 16);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) return false;
    out = h;
    return true;
}

// `asset` is a content path hashed with fnv1a64, or `asset#` holding 16 hex digits. False on a malformed hex key.
bool readAsset(const fmt::OcPlacedComponent& r, u64& id) {
    id = 0;
    if (const std::string* v = r.find("asset"); v && !v->empty()) {
        id = fnv1a64(std::string_view(slashed(*v)));
        return true;
    }
    if (const std::string* v = r.find("asset#"); v && !v->empty()) return parseHex(*v, id);
    return true;
}

bool readSurface(const fmt::OcPlacedComponent& r, i32& out, std::string& why) {
    out = 0;
    const std::string* v = r.find("surface");
    if (!v || v->empty()) return true;
    if ((*v)[0] == '#') {
        u64 h = 0;
        if (!parseHex(std::string_view(*v).substr(1), h) || h > 0xFFFFFFFFull) { why = "malformed surface '" + *v + "'"; return false; }
        out = static_cast<i32>(static_cast<u32>(h));
        return true;
    }
    if (!fmt::ocSurfNameValid(*v)) { why = "bad surface name '" + *v + "'"; return false; }
    out = aver::surfaceIdFromName(*v);
    return true;
}

// The record's spelling of a surface id: its registered name, else `#<hex>` so an id the table does not know survives the file.
std::string surfaceKeyOf(i32 id) {
#if AVER_MODULE_PHYSICS
    uint8_t name[96] = {};
    if (aver_phys_surface_info(id, nullptr, nullptr, nullptr, name, static_cast<i32>(sizeof name)) == 1 && name[0] != 0)
        return reinterpret_cast<const char*>(name);
#endif
    return "#" + hexOf(static_cast<u32>(id), 8);
}

} // namespace

u32 collisionShapeType(scene::World& world) { return world.componentId(kTypeName); }

u32 registerCollisionShapeComponent(scene::World& world) {
    if (const u32 have = world.componentId(kTypeName)) return have;
    using scene::FieldKind;
    auto b = world.registerComponent<CCollisionShape>(kTypeName);
    b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CCollisionShape, asset)))
        .field("surface", FieldKind::I32, static_cast<u16>(offsetof(CCollisionShape, surface)))
        .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CCollisionShape, flags)))
        .field("outShape", FieldKind::I32, static_cast<u16>(offsetof(CCollisionShape, outShape)), 0, true)
        .field("outMassKg", FieldKind::F32, static_cast<u16>(offsetof(CCollisionShape, outMassKg)), 0, true)
        .field("outVolumeCm3", FieldKind::F32, static_cast<u16>(offsetof(CCollisionShape, outVolumeCm3)), 0, true)
        .field("reserved", FieldKind::I32, static_cast<u16>(offsetof(CCollisionShape, reserved)), 0, true);
    AVER_ASSERTM(b.verify(sizeof(CCollisionShape)), "CCollisionShape");
    return b.typeId();
}

bool applyCollisionShape(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    if (!w.valid(e)) { why = "the entity is gone"; return false; }
    // A vehicle is its own rigid body (the same fight LevelComponents refuses the other way round).
    if (const u32 vehicle = w.componentId("CVehicle"); vehicle != 0 && w.hasComponent(e, vehicle)) { why = "the entity is a Vehicle: it is its own rigid body"; return false; }
    // Everything is read and checked before anything is attached, so a refusal never leaves half a component.
    u64 asset = 0;
    if (!readAsset(r, asset)) { why = "malformed asset#"; return false; }
    i32 surface = 0;
    if (!readSurface(r, surface, why)) return false;
    double noscale = 0.0;
    if (const std::string* v = r.find("noscale")) parseNum(*v, noscale);
    i32 motion = 0;   // an absent key is the canonical default, static: the record is the whole authored state (undo relies on it)
    if (const std::string* v = r.find("motion")) {
        std::string m = *v;
        for (char& ch : m) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (m == "static") motion = 0;
        else if (m == "kinematic") motion = 1;
        else if (m == "dynamic") motion = 2;
        else { why = "unknown motion '" + *v + "' (static, kinematic or dynamic)"; return false; }
    }

    const u32 type = registerCollisionShapeComponent(w);
    if (type == 0) { why = "CCollisionShape is not registered"; return false; }
    auto* c = w.component<CCollisionShape>(e, type);
    if (!c) {
        c = static_cast<CCollisionShape*>(w.addComponent(e, type));
        if (!c) { why = "the world refused the component"; return false; }
        *c = CCollisionShape{};
    }
    c->asset = asset;
    c->surface = surface;
    c->flags = (c->flags & ~kCollisionShapeIgnoreScale) | (noscale != 0.0 ? kCollisionShapeIgnoreScale : 0u);

    // The body's motion type, mass, friction and layer come from a CRigidBody, and no level file carries one: a placement that names a
    // shape gets a default (static) one, which `motion=` or the editor's Details can then change.
    if (!w.hasComponent(e, scene::kComponentRigidBody)) {
        if (auto* rb = static_cast<scene::CRigidBody*>(w.addComponent(e, scene::kComponentRigidBody))) *rb = scene::CRigidBody{};
    }
    if (auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody)) rb->motionType = motion;
    return true;
}

std::optional<fmt::OcPlacedComponent> captureCollisionShape(scene::World& w, scene::Entity e, const std::function<std::string(u64)>& pathOfId) {
    const u32 type = collisionShapeType(w);
    if (type == 0 || !w.valid(e) || !w.hasComponent(e, type)) return std::nullopt;
    const auto* c = w.component<CCollisionShape>(e, type);
    if (!c) return std::nullopt;
    fmt::OcPlacedComponent rec;
    rec.type = "CollisionShape";
    if (c->asset != 0) {
        const std::string p = pathOfId ? pathOfId(c->asset) : std::string{};
        if (!p.empty()) rec.kv.emplace_back("asset", p);
        else rec.kv.emplace_back("asset#", hexOf(c->asset, 16));
    }
    if (c->surface != 0) rec.kv.emplace_back("surface", surfaceKeyOf(c->surface));
    if (const auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody)) {
        if (rb->motionType == 1) rec.kv.emplace_back("motion", "kinematic");
        else if (rb->motionType == 2) rec.kv.emplace_back("motion", "dynamic");
    }
    if (c->flags & kCollisionShapeIgnoreScale) rec.kv.emplace_back("noscale", "1");
    return rec;
}

bool removeCollisionShape(scene::World& w, scene::Entity e) {
    const u32 type = collisionShapeType(w);
    if (type == 0 || !w.valid(e) || !w.hasComponent(e, type)) return false;
    if (!w.removeComponent(e, type)) return false;
    // The body applyCollisionShape attached is a zero-size box until something sizes it, and a body nobody built from: taking it back
    // makes the undo of Add Component exact. A Rigid Body with a size, or a live body, is somebody else's and stays.
    if (const auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody)) {
        const bool pristine = rb->body == 0 && rb->shapeKind == scene::kRigidBodyShapeBox && rb->dims[0] == 0.0f && rb->dims[1] == 0.0f && rb->dims[2] == 0.0f;
        if (pristine) w.removeComponent(e, scene::kComponentRigidBody);
    }
    return true;
}

#endif // AVER_MODULE_SCENE

} // namespace aver::game
