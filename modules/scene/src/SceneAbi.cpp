// The scene C ABI's translation unit: every exported entry point, and the DLL-local intern tables
// the material and string accessors need.
#include "aver/scene/scene_abi.h"

#include "aver/scene/Components.hpp"
#include "aver/scene/Fields.hpp"
#include "aver/scene/World.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::scene;

namespace {

// The KIND/COMP defines are the ABI's copy of FieldKind and the kComponent* ids; these catch drift.
static_assert(AVER_SCENE_KIND_F32    == static_cast<int>(FieldKind::F32),    "KIND_F32 drift");
static_assert(AVER_SCENE_KIND_VEC3   == static_cast<int>(FieldKind::Vec3),   "KIND_VEC3 drift");
static_assert(AVER_SCENE_KIND_QUAT   == static_cast<int>(FieldKind::Quat),   "KIND_QUAT drift");
static_assert(AVER_SCENE_KIND_I32    == static_cast<int>(FieldKind::I32),    "KIND_I32 drift");
static_assert(AVER_SCENE_KIND_BOOL   == static_cast<int>(FieldKind::Bool),   "KIND_BOOL drift");
static_assert(AVER_SCENE_KIND_I64    == static_cast<int>(FieldKind::I64),    "KIND_I64 drift");
static_assert(AVER_SCENE_KIND_ENTITY == static_cast<int>(FieldKind::Entity), "KIND_ENTITY drift");
static_assert(AVER_SCENE_KIND_STRING == static_cast<int>(FieldKind::String), "KIND_STRING drift");
static_assert(AVER_SCENE_KIND_MAT4   == static_cast<int>(FieldKind::Mat4),   "KIND_MAT4 drift");

static_assert(AVER_SCENE_COMP_LOCAL         == kComponentLocal,        "COMP_LOCAL drift");
static_assert(AVER_SCENE_COMP_WORLD         == kComponentWorld,        "COMP_WORLD drift");
static_assert(AVER_SCENE_COMP_HIERARCHY     == kComponentHierarchy,    "COMP_HIERARCHY drift");
static_assert(AVER_SCENE_COMP_NAME          == kComponentName,         "COMP_NAME drift");
static_assert(AVER_SCENE_COMP_TAGS          == kComponentTags,         "COMP_TAGS drift");
static_assert(AVER_SCENE_COMP_MESH_RENDERER == kComponentMeshRenderer, "COMP_MESH_RENDERER drift");
static_assert(AVER_SCENE_COMP_LIGHT         == kComponentLight,        "COMP_LIGHT drift");
static_assert(AVER_SCENE_COMP_CAMERA        == kComponentCamera,       "COMP_CAMERA drift");
static_assert(AVER_SCENE_COMP_SKELETAL_MESH == kComponentSkeletalMesh, "COMP_SKELETAL_MESH drift");
static_assert(AVER_SCENE_COMP_ANIMATOR      == kComponentAnimator,     "COMP_ANIMATOR drift");
static_assert(AVER_SCENE_COMP_PARTICLE_EMITTER == kComponentParticleEmitter, "COMP_PARTICLE_EMITTER drift");
static_assert(AVER_SCENE_COMP_ATTACHMENT == kComponentAttachment, "COMP_ATTACHMENT drift");

// The single process-global world.
World& world() { return World::instance(); }

// Reinterprets an int32_t from the ABI as an entity handle.
Entity toEntity(int32_t e) { return static_cast<Entity>(static_cast<uint32_t>(e)); }

// Resolves a field id to its descriptor, or nullptr for 0 / an unknown id.
const FieldDesc* desc(int32_t f) { return world().field(static_cast<u32>(f)); }

// The byte address of field `f` inside entity `e`'s component, or nullptr on any rejection.
u8* fieldAddr(int32_t e, const FieldDesc* d) {
    if (!d) return nullptr;
    void* base = world().getComponent(toEntity(e), d->component);
    return base ? static_cast<u8*>(base) + d->offset : nullptr;
}

// Bumps the transform revision after a write to any CLocal field.
void noteWrite(int32_t e, const FieldDesc* d) {
    if (d && d->component == kComponentLocal) world().touchLocal(toEntity(e));
}

// Material name -> opaque i32 token, keyed by "packId/name". DLL-local; the render side maps it back.
std::unordered_map<std::string, int32_t>& materialTable() {
    static std::unordered_map<std::string, int32_t> table;
    return table;
}

// The inverse, indexed by token-1, so aver_scene_material_name is O(1) rather than a scan.
//
// A DEQUE AND NOT A VECTOR, on purpose: this hands out `const char*` into its elements, and a vector
// reallocating would move every short string that lives inside its own object under SSO -- leaving
// every pointer already returned dangling. A deque never moves an element it has stored.
//
// Holds the NAME only, not the "packId/name" key: the pack id is the caller's and it already has it.
std::deque<std::string>& materialNames() {
    static std::deque<std::string> names;
    return names;
}

// The interned strings a String field's 8 bytes index into. DLL-local.
std::vector<std::string>& stringPool() {
    static std::vector<std::string> pool{std::string()};   // index 0 == the empty string
    return pool;
}

}  // namespace

extern "C" {

// Returns AVER_SCENE_ABI_VERSION as compiled into this DLL.
int32_t aver_scene_abi_version(void) {
    return AVER_SCENE_ABI_VERSION;
}

// Number of interned strings. A test hook, deliberately not declared in scene_abi.h.
AVER_SCENE_ABI int64_t aver_scene_debug_string_pool_size(void) {
    return static_cast<int64_t>(stringPool().size());
}

// ---- field resolution ------------------------------------------------------------------------------

// Resolves "Component.field" to a dense field id, or 0.
int32_t aver_scene_field(const char* qualifiedName) {
    if (!qualifiedName) return 0;
    return static_cast<int32_t>(world().fieldId(qualifiedName));
}

// The AVER_SCENE_KIND_* of a field id.
int32_t aver_scene_field_kind(int32_t f) {
    const FieldDesc* d = desc(f);
    return d ? static_cast<int32_t>(d->kind) : 0;
}

// Floats per value for a float-kind field, 0 for the rest.
int32_t aver_scene_field_arity(int32_t f) {
    const FieldDesc* d = desc(f);
    return d ? static_cast<int32_t>(d->arity) : 0;
}

// ---- f32 -------------------------------------------------------------------------------------------

// Reads an F32 field. 0.0f on any rejection.
float aver_scene_get_f32(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::F32) return 0.0f;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0.0f;
    float v;
    std::memcpy(&v, a, sizeof(v));
    return v;
}

// Writes an F32 field. 0 on any rejection.
int32_t aver_scene_set_f32(int32_t e, int32_t f, float v) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::F32 || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(a, &v, sizeof(v));
    noteWrite(e, d);
    return 1;
}

// ---- vec (any float kind: F32/Vec3/Quat/Mat4) ------------------------------------------------------

// Reads any float-kind field into `outv`, which must hold `arity` floats.
int32_t aver_scene_get_vec(int32_t e, int32_t f, float* outv) {
    if (!outv) return 0;
    const FieldDesc* d = desc(f);
    if (!d || d->arity == 0) return 0;   // arity 0 == not a float kind, so it is the wrong family
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(outv, a, sizeof(float) * d->arity);
    return 1;
}

// Writes any float-kind field from `arity` floats.
int32_t aver_scene_set_vec(int32_t e, int32_t f, const float* v) {
    if (!v) return 0;
    const FieldDesc* d = desc(f);
    if (!d || d->arity == 0 || d->readOnly) return 0;   // CWorld.matrix is a float kind but read-only
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(a, v, sizeof(float) * d->arity);
    noteWrite(e, d);
    return 1;
}

// ---- i32 (also Bool) -------------------------------------------------------------------------------

// Reads an I32 or Bool field. 0 on any rejection.
int32_t aver_scene_get_i32(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || (d->kind != FieldKind::I32 && d->kind != FieldKind::Bool)) return 0;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    int32_t v;
    std::memcpy(&v, a, sizeof(v));
    return v;
}

// Writes an I32 or Bool field. 0 on any rejection.
int32_t aver_scene_set_i32(int32_t e, int32_t f, int32_t v) {
    const FieldDesc* d = desc(f);
    if (!d || (d->kind != FieldKind::I32 && d->kind != FieldKind::Bool) || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(a, &v, sizeof(v));
    noteWrite(e, d);
    return 1;
}

// ---- i64 (the ObjectId / mesh family) --------------------------------------------------------------

// Reads an I64 field. 0 on any rejection.
int64_t aver_scene_get_i64(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::I64) return 0;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    int64_t v;
    std::memcpy(&v, a, sizeof(v));
    return v;
}

// Writes an I64 field. 0 on any rejection.
int32_t aver_scene_set_i64(int32_t e, int32_t f, int64_t v) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::I64 || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(a, &v, sizeof(v));
    noteWrite(e, d);
    return 1;
}

// ---- ref (Entity — its own kind) -------------------------------------------------------------------

// Reads an Entity field. 0 on any rejection.
int32_t aver_scene_get_ref(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::Entity) return 0;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    Entity v;
    std::memcpy(&v, a, sizeof(v));
    return static_cast<int32_t>(v);   // bit 31 clear, so a live entity crosses positive
}

// Writes an Entity field. 0 on any rejection; CHierarchy's links are read-only, use set_parent.
int32_t aver_scene_set_ref(int32_t e, int32_t f, int32_t v) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::Entity || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    const Entity ref = toEntity(v);
    std::memcpy(a, &ref, sizeof(ref));
    noteWrite(e, d);
    return 1;
}

// ---- str -------------------------------------------------------------------------------------------

// Reads a String field as an interned pointer the caller must not free. "" on any rejection.
const char* aver_scene_get_str(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::String) return "";
    const u8* a = fieldAddr(e, d);
    if (!a) return "";
    int64_t id;
    std::memcpy(&id, a, sizeof(id));
    const auto& pool = stringPool();
    if (id <= 0 || static_cast<size_t>(id) >= pool.size()) return "";
    return pool[static_cast<size_t>(id)].c_str();
}

// Writes a String field, reusing the intern slot the field already owns. 0 on any rejection.
int32_t aver_scene_set_str(int32_t e, int32_t f, const char* v) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::String || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;

    auto&         pool = stringPool();
    int64_t       id   = 0;
    std::memcpy(&id, a, sizeof(id));
    const bool ownsSlot = id >= 1 && static_cast<size_t>(id) < pool.size();
    if (v && *v) {
        if (ownsSlot) {
            pool[static_cast<size_t>(id)] = v;
        } else {
            pool.emplace_back(v);
            id = static_cast<int64_t>(pool.size() - 1);
        }
    } else if (ownsSlot) {
        pool[static_cast<size_t>(id)].clear();   // keep the slot
    }
    std::memcpy(a, &id, sizeof(id));
    noteWrite(e, d);
    return 1;
}

// ---- entity lifetime + hierarchy -------------------------------------------------------------------

// Creates an unnamed entity. 0 if none could be made.
int32_t aver_scene_create(void) {
    return static_cast<int32_t>(world().create(std::string_view{}));
}

// Queues the entity and its subtree for destruction at the next flush.
int32_t aver_scene_destroy(int32_t e) {
    return world().destroy(toEntity(e)) ? 1 : 0;
}

// 1 when the handle addresses a live entity.
int32_t aver_scene_valid(int32_t e) {
    return world().valid(toEntity(e)) ? 1 : 0;
}

// Resolves a component type by registered name. 0 when unregistered -- which addComponent below
// then rejects, so a typo fails closed rather than attaching some other pool.
int32_t aver_scene_component(const char* name) {
    if (!name) return 0;
    return static_cast<int32_t>(world().componentId(name));
}

// Attaches a component's storage to the entity.
int32_t aver_scene_add_component(int32_t e, int32_t component) {
    return world().addComponent(toEntity(e), static_cast<u32>(component)) ? 1 : 0;
}

// Reparents `child`. parent 0 makes it a root.
int32_t aver_scene_set_parent(int32_t child, int32_t parent) {
    return world().setParent(toEntity(child), toEntity(parent)) ? 1 : 0;
}

// ---- persisted identity ----------------------------------------------------------------------------

// The entity's persisted identity. 0 for a stale handle.
int64_t aver_scene_object_id(int32_t e) {
    return static_cast<int64_t>(world().objectId(toEntity(e)));
}

// Overrides the entity's persisted identity.
int32_t aver_scene_set_object_id(int32_t e, int64_t objectId) {
    return world().setObjectId(toEntity(e), static_cast<u64>(objectId)) ? 1 : 0;
}

// The entity's name, pointing into the world's name blob. The caller must not free it.
const char* aver_scene_name(int32_t e) {
    return world().name(toEntity(e));
}

// Renames the entity.
int32_t aver_scene_set_name(int32_t e, const char* name) {
    return world().setName(toEntity(e), name ? std::string_view(name) : std::string_view{}) ? 1 : 0;
}

// ---- query -----------------------------------------------------------------------------------------

// The first live entity with this name, or 0.
int32_t aver_scene_find(const char* name) {
    if (!name) return 0;
    return static_cast<int32_t>(world().find(std::string_view(name)));
}

// Writes the entity's world matrix into out16. Row-major, translation in row 3.
int32_t aver_scene_world_matrix(int32_t e, float* out16) {
    const Entity ent = toEntity(e);
    if (!out16 || !world().valid(ent)) return 0;
    const Mat4& m = world().worldMatrix(ent);
    std::memcpy(out16, &m.m[0][0], sizeof(float) * 16);
    return 1;
}

// 1 when the entity carries this component.
int32_t aver_scene_has_component(int32_t e, int32_t component) {
    return world().hasComponent(toEntity(e), static_cast<u32>(component)) ? 1 : 0;
}

// The entity's parent, or 0.
int32_t aver_scene_parent(int32_t e) {
    return static_cast<int32_t>(world().parent(toEntity(e)));
}

// The entity's first child, or 0.
int32_t aver_scene_first_child(int32_t e) {
    return static_cast<int32_t>(world().firstChild(toEntity(e)));
}

// The next sibling under the same parent, or 0.
int32_t aver_scene_next_sibling(int32_t e) {
    return static_cast<int32_t>(world().nextSibling(toEntity(e)));
}

// Number of immediate children.
int32_t aver_scene_child_count(int32_t e) {
    return static_cast<int32_t>(world().childCount(toEntity(e)));
}

// Number of live entities.
int32_t aver_scene_count(void) {
    return static_cast<int32_t>(world().count());
}

// The live entity at a dense index, or 0 out of range.
int32_t aver_scene_at(int32_t index) {
    if (index < 0 || static_cast<u32>(index) >= world().count()) return 0;
    return static_cast<int32_t>(world().at(static_cast<u32>(index)));
}

// ---- content resolution ----------------------------------------------------------------------------

// Interns a material name under content-pack `name0` and returns its opaque i32 token. 0 for empty.
int32_t aver_scene_material(int32_t name0, const char* name) {
    if (!name || !*name) return 0;
    std::string key = std::to_string(name0);
    key.push_back('/');
    key.append(name);

    auto& table = materialTable();
    const auto it = table.find(key);
    if (it != table.end()) return it->second;

    const int32_t handle = static_cast<int32_t>(table.size()) + 1;   // sequential from 1
    table.emplace(std::move(key), handle);
    // Kept in lockstep, so materialNames()[handle - 1] is always this name. Appending here and
    // nowhere else is what keeps the two in step -- there is no other path that mints a token.
    materialNames().emplace_back(name);
    return handle;
}

// The name a token was interned under. "" for 0 or anything out of range; never NULL.
const char* aver_scene_material_name(int32_t token) {
    if (token <= 0) return "";
    const std::deque<std::string>& names = materialNames();
    const std::size_t i = static_cast<std::size_t>(token) - 1;
    if (i >= names.size()) return "";
    return names[i].c_str();
}

}  // extern "C"
