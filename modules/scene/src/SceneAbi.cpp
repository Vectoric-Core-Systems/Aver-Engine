#include "aver/scene/scene_abi.h"

#include "aver/scene/Components.hpp"
#include "aver/scene/Fields.hpp"
#include "aver/scene/World.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// The C ABI's translation unit. The exported functions are `extern "C"` and hold no C++ in their
// signatures, so this file stays a stable binary surface as the module behind it grows. Unlike the
// version stub this file used to be, it now includes the C++ World headers: void* and std types cross
// only WITHIN this DLL, between this TU and the World it was compiled with — never across the C ABI.

using namespace aver;
using namespace aver::scene;

namespace {

// ---- the KIND/COMP defines are the ABI's copy of two C++ enums; drift here is a wrong dispatch, so
// it is caught at compile time rather than at a mis-read field. -----------------------------------
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

// The single process-global world, created lazily on first use. World::instance() is already a
// function-local static, so this indirection is only here to name the design decision in one place.
World& world() { return World::instance(); }

// An int32_t entity crosses as its raw handle bits; a live one has bit 31 clear so this reinterpret is
// total and 0 stays invalid. A negative int (which no live entity can be) becomes a huge index that
// valid()/getComponent reject, so no bounds special-case is needed.
Entity toEntity(int32_t e) { return static_cast<Entity>(static_cast<uint32_t>(e)); }

// Resolve a field id to its descriptor, or nullptr for 0 / an unknown id.
const FieldDesc* desc(int32_t f) { return world().field(static_cast<u32>(f)); }

// The byte address of field `f` inside entity `e`'s component, or nullptr when the handle is stale,
// the field is unknown, or the entity does not carry that component. getComponent already returns
// nullptr for a stale handle (the pool's owner-check) and for an absent component, so this one guard
// covers every rejection path the accessors share.
u8* fieldAddr(int32_t e, const FieldDesc* d) {
    if (!d) return nullptr;
    void* base = world().getComponent(toEntity(e), d->component);
    return base ? static_cast<u8*>(base) + d->offset : nullptr;
}

// A write to a CLocal field (position/rotation/scale/rev) must bump CLocal::rev, or the world-matrix
// pass — which is a revision compare, not a dirty-bit walk — never sees it. touchLocal is exactly the
// hook World documents for "a caller wrote the transform through the pool directly".
void noteWrite(int32_t e, const FieldDesc* d) {
    if (d && d->component == kComponentLocal) world().touchLocal(toEntity(e));
}

// ---- material name intern table --------------------------------------------------------------------
// Materials belong to Aver.Render.PBR, which this DLL MUST NOT link. So a material NAME is resolved to
// a stable, positive i32 that is meaningful only as a token: the render side owns the map from this
// token back to a real material. Keyed by (content-pack id, name) so the same name in two packs gets
// two handles. Ids are handed out sequentially from 1; 0 is reserved for the empty name. Not a hash of
// the bytes on purpose — a monotone id cannot collide, and stability within a process is all a bind
// pass needs.
std::unordered_map<std::string, int32_t>& materialTable() {
    static std::unordered_map<std::string, int32_t> table;
    return table;
}

// ---- string-field intern table ---------------------------------------------------------------------
// A String field occupies 8 bytes (fieldByteSize(String) == 8). The design's intent is an {offset,len}
// slice into the world name blob, but the world exposes no public primitive to append an arbitrary
// field's bytes to that blob, so the ABI cannot reach it. Instead the 8 bytes hold an int64 intern id
// into a table local to this DLL, and get_str hands back the interned pointer. This keeps the str
// accessor family honest and self-contained; see the report note for why this is a scene-side store
// rather than the name blob. (No built-in field is String-kind today, so this path serves only
// script-declared String fields and the editor's Details panel.)
std::vector<std::string>& stringPool() {
    static std::vector<std::string> pool{std::string()};   // index 0 == the empty string
    return pool;
}

}  // namespace

extern "C" {

int32_t aver_scene_abi_version(void) {
    // Compiled INTO the DLL, so it reports what this binary was built with rather than what the
    // caller's header says. A caller comparing the two is the only way a stale DLL is caught.
    return AVER_SCENE_ABI_VERSION;
}

// Test/introspection hook, deliberately NOT declared in scene_abi.h: the count of interned strings.
// It exists so a same-process test can prove that repeated set_str into one field reuses its slot
// instead of leaking one pool entry per write (the intern table is otherwise invisible over the ABI).
AVER_SCENE_ABI int64_t aver_scene_debug_string_pool_size(void) {
    return static_cast<int64_t>(stringPool().size());
}

// ---- field resolution ------------------------------------------------------------------------------

int32_t aver_scene_field(const char* qualifiedName) {
    if (!qualifiedName) return 0;
    return static_cast<int32_t>(world().fieldId(qualifiedName));
}

int32_t aver_scene_field_kind(int32_t f) {
    const FieldDesc* d = desc(f);
    return d ? static_cast<int32_t>(d->kind) : 0;
}

int32_t aver_scene_field_arity(int32_t f) {
    const FieldDesc* d = desc(f);
    return d ? static_cast<int32_t>(d->arity) : 0;
}

// ---- f32 -------------------------------------------------------------------------------------------

float aver_scene_get_f32(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::F32) return 0.0f;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0.0f;
    float v;
    std::memcpy(&v, a, sizeof(v));
    return v;
}

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

int32_t aver_scene_get_vec(int32_t e, int32_t f, float* outv) {
    if (!outv) return 0;
    const FieldDesc* d = desc(f);
    if (!d || d->arity == 0) return 0;   // arity 0 == not a float kind, so it is the wrong family
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(outv, a, sizeof(float) * d->arity);
    return 1;
}

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

int32_t aver_scene_get_i32(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || (d->kind != FieldKind::I32 && d->kind != FieldKind::Bool)) return 0;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    int32_t v;
    std::memcpy(&v, a, sizeof(v));
    return v;
}

int32_t aver_scene_set_i32(int32_t e, int32_t f, int32_t v) {
    const FieldDesc* d = desc(f);
    // read-only rejects CName.offset/len (a written slice cursor is the OOB name read this closes),
    // CWorld's revisions, CHierarchy.depth, CMeshRenderer.dirty.
    if (!d || (d->kind != FieldKind::I32 && d->kind != FieldKind::Bool) || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    std::memcpy(a, &v, sizeof(v));
    noteWrite(e, d);
    return 1;
}

// ---- i64 (the ObjectId / mesh family) --------------------------------------------------------------

int64_t aver_scene_get_i64(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::I64) return 0;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    int64_t v;
    std::memcpy(&v, a, sizeof(v));
    return v;
}

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

int32_t aver_scene_get_ref(int32_t e, int32_t f) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::Entity) return 0;
    const u8* a = fieldAddr(e, d);
    if (!a) return 0;
    Entity v;
    std::memcpy(&v, a, sizeof(v));
    return static_cast<int32_t>(v);   // bit 31 clear, so a live entity crosses positive
}

int32_t aver_scene_set_ref(int32_t e, int32_t f, int32_t v) {
    const FieldDesc* d = desc(f);
    // CHierarchy's Entity fields (parent/firstChild/nextSibling/prevSibling) are the world's structural
    // links, marked read-only for exactly this reason: World::setParent keeps them acyclic (it refuses
    // self-parenting and walks the ancestor chain), and a raw byte write bypasses all of it — a single
    // set_ref(e, CHierarchy.parent, e), or a pair forming a 2-cycle, makes composeChain()/worldMatrix()
    // loop with no visited guard and grow unbounded (hang/bad_alloc). Structural edits go through
    // aver_scene_set_parent; get_ref on these fields (a read) is unaffected.
    if (!d || d->kind != FieldKind::Entity || d->readOnly) return 0;
    u8* a = fieldAddr(e, d);
    if (!a) return 0;
    const Entity ref = toEntity(v);
    std::memcpy(a, &ref, sizeof(ref));
    noteWrite(e, d);
    return 1;
}

// ---- str -------------------------------------------------------------------------------------------

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

int32_t aver_scene_set_str(int32_t e, int32_t f, const char* v) {
    const FieldDesc* d = desc(f);
    if (!d || d->kind != FieldKind::String || d->readOnly) return 0;   // wrong-kind / read-only rejection
    u8* a = fieldAddr(e, d);
    if (!a) return 0;

    // Reuse the slot this field already owns rather than appending a fresh one every write. Without
    // this the process-global pool grows by one std::string per set_str — editing one Details-panel
    // field N times leaks N-1 entries into a never-reclaimed vector. A field starts zero-filled (id 0,
    // no slot), so the first non-empty write still appends; every write after that overwrites in place,
    // which also keeps get_str's returned pointer stable across repeated edits of the same field.
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
        pool[static_cast<size_t>(id)].clear();   // keep the slot; store empty rather than abandoning it
    }
    std::memcpy(a, &id, sizeof(id));
    noteWrite(e, d);
    return 1;
}

// ---- entity lifetime + hierarchy -------------------------------------------------------------------

int32_t aver_scene_create(void) {
    // Created unnamed; the framework names it via aver_scene_set_name when it wires an actor.
    return static_cast<int32_t>(world().create(std::string_view{}));
}

int32_t aver_scene_destroy(int32_t e) {
    return world().destroy(toEntity(e)) ? 1 : 0;
}

int32_t aver_scene_valid(int32_t e) {
    return world().valid(toEntity(e)) ? 1 : 0;
}

int32_t aver_scene_add_component(int32_t e, int32_t component) {
    return world().addComponent(toEntity(e), static_cast<u32>(component)) ? 1 : 0;
}

int32_t aver_scene_set_parent(int32_t child, int32_t parent) {
    // parent 0 == kInvalidEntity, which World::setParent reads as "make this a root".
    return world().setParent(toEntity(child), toEntity(parent)) ? 1 : 0;
}

// ---- persisted identity ----------------------------------------------------------------------------

int64_t aver_scene_object_id(int32_t e) {
    return static_cast<int64_t>(world().objectId(toEntity(e)));
}

int32_t aver_scene_set_object_id(int32_t e, int64_t objectId) {
    return world().setObjectId(toEntity(e), static_cast<u64>(objectId)) ? 1 : 0;
}

const char* aver_scene_name(int32_t e) {
    // Points into the world's name blob; the caller decodes it immediately and must not free it.
    return world().name(toEntity(e));
}

int32_t aver_scene_set_name(int32_t e, const char* name) {
    return world().setName(toEntity(e), name ? std::string_view(name) : std::string_view{}) ? 1 : 0;
}

// ---- query -----------------------------------------------------------------------------------------

int32_t aver_scene_find(const char* name) {
    if (!name) return 0;
    return static_cast<int32_t>(world().find(std::string_view(name)));
}

int32_t aver_scene_world_matrix(int32_t e, float* out16) {
    const Entity ent = toEntity(e);
    if (!out16 || !world().valid(ent)) return 0;
    const Mat4& m = world().worldMatrix(ent);   // composes on demand; row-major, translation in row 3
    std::memcpy(out16, &m.m[0][0], sizeof(float) * 16);
    return 1;
}

// ---- content resolution ----------------------------------------------------------------------------

int32_t aver_scene_material(int32_t name0, const char* name) {
    if (!name || !*name) return 0;
    // Fold the pack id into the key so the same name in two packs is two handles. A DLL-local intern,
    // never a call into Aver.Render.PBR — the render side is what interprets the returned token.
    std::string key = std::to_string(name0);
    key.push_back('/');
    key.append(name);

    auto& table = materialTable();
    const auto it = table.find(key);
    if (it != table.end()) return it->second;

    const int32_t handle = static_cast<int32_t>(table.size()) + 1;   // sequential from 1; 0 stays "none"
    table.emplace(std::move(key), handle);
    return handle;
}

}  // extern "C"
