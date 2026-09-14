#ifndef AVER_SCENE_ABI_H
#define AVER_SCENE_ABI_H

/* Scene C ABI — the stable surface the C# scripting layer binds to via P/Invoke.
 *
 * Only int32_t / int64_t / float / const char* cross; vectors go through float* out-params. Setters
 * return 1 on success and 0 on rejection; getters return a documented neutral for a stale handle. */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_SCENE_BUILD)
#    define AVER_SCENE_ABI __declspec(dllexport)
#  else
#    define AVER_SCENE_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_SCENE_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version, as (major << 16) | minor. Major changes break a binding; minor only adds. */
/* MINOR 1 adds aver_scene_material_name. Additive only -- no existing binding changes. */
/* MINOR 2 adds AVER_SCENE_COMP_PARTICLE_EMITTER (11th built-in). Additive only. */
/* MINOR 3 adds AVER_SCENE_COMP_ATTACHMENT (12th built-in). Additive only. */
/* MINOR 4 adds aver_scene_component, which resolves a DYNAMICALLY registered component type by
 * name. Additive only -- the AVER_SCENE_COMP_* built-ins keep their fixed ids and every existing
 * binding is unchanged. */
/* MINOR 5 adds aver_scene_last_error, the reason channel. Additive only -- every existing
 * entry point keeps its 1/0 return exactly, which is the whole design; see that function. */
#define AVER_SCENE_ABI_VERSION_MAJOR 1
#define AVER_SCENE_ABI_VERSION_MINOR 5
#define AVER_SCENE_ABI_VERSION \
    ((AVER_SCENE_ABI_VERSION_MAJOR << 16) | AVER_SCENE_ABI_VERSION_MINOR)

/* Returns AVER_SCENE_ABI_VERSION as the DLL was BUILT with, so a caller can catch a stale binary. */
AVER_SCENE_ABI int32_t aver_scene_abi_version(void);

/* WHY the last call failed, as an aver::AbiError (modules/core/include/aver/core/ErrorCodes.hpp):
   0 ok, -1 bad handle, -2 null pointer, -4 out of range, -5 unsupported, -6 invalid argument.
   Every code except 0 is negative, so `< 0` means "failed" even for one added after your binding.

   A SEPARATE ENTRY POINT, not a changed return value, and that is the load-bearing decision. Every
   setter here returns 1/0 and every caller writes `if (aver_scene_set_f32(...))`; a negative code on
   those returns would be TRUE, silently inverting each of those call sites with no compile error.

   WHAT IT SEPARATES. `aver_scene_set_f32` returning 0 currently means one of four different things
   and says which of them nowhere: no field has that id (-1), the entity has no such component (-1),
   the field is real but of another kind (-6), or the field is real, of the right kind, and READ-ONLY
   (-5) -- CWorld.matrix being the case that has actually confused people.

   THREAD-LOCAL, and per-DLL: it reports scene failures only. Aver.Core is linked statically into
   each ABI DLL, so this slot and aver_phys_last_error's are different slots.

   SET ON SUCCESS TOO (to 0), so a stale reason cannot outlive the failure that produced it. A call
   that resolves neither a field nor an entity leaves it alone. */
AVER_SCENE_ABI int32_t aver_scene_last_error(void);

/* FIELD KINDS — pinned one-for-one to aver::scene::FieldKind in Fields.hpp. */
#define AVER_SCENE_KIND_F32    0
#define AVER_SCENE_KIND_VEC3   1
#define AVER_SCENE_KIND_QUAT   2
#define AVER_SCENE_KIND_I32    3
#define AVER_SCENE_KIND_BOOL   4
#define AVER_SCENE_KIND_I64    5
#define AVER_SCENE_KIND_ENTITY 6
#define AVER_SCENE_KIND_STRING 7
#define AVER_SCENE_KIND_MAT4   8

/* FIXED COMPONENT IDS — pinned to aver::scene::kComponent* in Components.hpp. Script-declared
 * components take ids above AVER_SCENE_COMP_ANIMATOR. */
#define AVER_SCENE_COMP_LOCAL         1
#define AVER_SCENE_COMP_WORLD         2
#define AVER_SCENE_COMP_HIERARCHY     3
#define AVER_SCENE_COMP_NAME          4
#define AVER_SCENE_COMP_TAGS          5
#define AVER_SCENE_COMP_MESH_RENDERER 6
#define AVER_SCENE_COMP_LIGHT         7
#define AVER_SCENE_COMP_CAMERA        8
#define AVER_SCENE_COMP_SKELETAL_MESH 9
#define AVER_SCENE_COMP_ANIMATOR      10
#define AVER_SCENE_COMP_PARTICLE_EMITTER 11
#define AVER_SCENE_COMP_ATTACHMENT    12

/* Every entry point below acts on the single process-global World; no world handle crosses.
 * Strings are UTF-8 both ways, and an OUT pointer is owned by the world and must NOT be freed.
 * Entities cross as int32_t: a live entity is always positive, and 0 is the only invalid value. */

/* ---- field resolution ---- */
/* Resolve "Component.field" (e.g. "CLocal.position") to a dense field id; 0 when unknown. */
AVER_SCENE_ABI int32_t aver_scene_field(const char* qualifiedName);
/* The AVER_SCENE_KIND_* of a field id, or 0 (== F32) for an unknown id. */
AVER_SCENE_ABI int32_t aver_scene_field_kind(int32_t f);
/* Floats-per-value for a float kind, 0 for the rest — what a vector accessor sizes its buffer from. */
AVER_SCENE_ABI int32_t aver_scene_field_arity(int32_t f);

/* ---- typed get/set, one family per kind ----
 * A get through the wrong family returns the neutral value; a set returns 0 and writes nothing.
 * Writing any CLocal field bumps the transform revision. */
/* Read/write a KIND_F32 field. */
AVER_SCENE_ABI float   aver_scene_get_f32(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_f32(int32_t e, int32_t f, float v);
/* Read/write any float kind; the buffer must hold aver_scene_field_arity floats. */
AVER_SCENE_ABI int32_t aver_scene_get_vec(int32_t e, int32_t f, float* outv);
AVER_SCENE_ABI int32_t aver_scene_set_vec(int32_t e, int32_t f, const float* v);
/* Read/write a KIND_I32 or KIND_BOOL field. */
AVER_SCENE_ABI int32_t aver_scene_get_i32(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_i32(int32_t e, int32_t f, int32_t v);
/* Read/write a KIND_I64 field. */
AVER_SCENE_ABI int64_t aver_scene_get_i64(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_i64(int32_t e, int32_t f, int64_t v);
/* Read/write a KIND_ENTITY field. An entity reference is its own kind, not an int32 in disguise. */
AVER_SCENE_ABI int32_t aver_scene_get_ref(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_ref(int32_t e, int32_t f, int32_t v);
/* Read/write a KIND_STRING field. */
AVER_SCENE_ABI const char* aver_scene_get_str(int32_t e, int32_t f);   /* "" for stale/wrong-kind */
AVER_SCENE_ABI int32_t     aver_scene_set_str(int32_t e, int32_t f, const char* v);

/* ---- entity lifetime + hierarchy ---- */
/* Create an unnamed entity (CName/CLocal/CWorld/CHierarchy attached at birth). 0 if none could be made. */
AVER_SCENE_ABI int32_t aver_scene_create(void);
/* Deferred to the next flush and takes the whole subtree; the handle stays valid for the rest of the frame. */
AVER_SCENE_ABI int32_t aver_scene_destroy(int32_t e);
/* 1 if the handle addresses a live entity, else 0 (rejects 0, a stale generation, and a freed slot). */
AVER_SCENE_ABI int32_t aver_scene_valid(int32_t e);
/* Resolve a component TYPE by its registered name (e.g. "CControlRig") to the id the two calls
 * below take; 0 when nothing of that name is registered.
 *
 * THIS IS THE ONLY WAY A BINDING CAN REACH A DYNAMICALLY REGISTERED COMPONENT. The AVER_SCENE_COMP_*
 * ids above are fixed and cover the built-ins only, so a type registered at runtime through
 * World::registerComponent -- CControlRig, CSynapseAgent and anything else that deliberately stays
 * out of Components.hpp -- has an id that is not knowable ahead of time and differs with
 * registration order. Fields of such a component were always reachable (aver_scene_field resolves
 * "CControlRig.rig" by name against the same registry), but ATTACHING one was not, which left every
 * dynamic component addable from C++ and from nowhere else. */
AVER_SCENE_ABI int32_t aver_scene_component(const char* name);

/* Attach a component pool's storage to `e`; returns 1 (idempotent), 0 if the type or handle is bad. */
AVER_SCENE_ABI int32_t aver_scene_add_component(int32_t e, int32_t component);
/* Reparent; parent 0 makes `child` a root. Refuses a cycle, a self-parent, or a doomed parent. */
AVER_SCENE_ABI int32_t aver_scene_set_parent(int32_t child, int32_t parent);

/* ---- persisted identity ---- */
/* Read/write CName.objectId. The world's u64 crosses as int64_t. */
AVER_SCENE_ABI int64_t aver_scene_object_id(int32_t e);               /* 0 for a stale handle */
AVER_SCENE_ABI int32_t aver_scene_set_object_id(int32_t e, int64_t objectId);
/* Read/write the entity's name. */
AVER_SCENE_ABI const char* aver_scene_name(int32_t e);               /* "" for a stale handle */
AVER_SCENE_ABI int32_t     aver_scene_set_name(int32_t e, const char* name);

/* ---- query ---- */
/* The first live entity whose name equals `name`, or 0. A linear scan, not a per-frame lookup. */
AVER_SCENE_ABI int32_t aver_scene_find(const char* name);
/* Write `e`'s 4x4 world matrix into out16: row-major, basis in rows 0-2, translation in row 3.
 * Composes on demand. Returns 0 leaving out16 untouched for a dead handle or null out. */
AVER_SCENE_ABI int32_t aver_scene_world_matrix(int32_t e, float* out16);

/* 1 if `e` currently carries component `component` (a AVER_SCENE_COMP_* id), else 0. */
AVER_SCENE_ABI int32_t aver_scene_has_component(int32_t e, int32_t component);

/* ---- hierarchy queries (the setters live in "entity lifetime" above) ---- */
/* Immediate parent of `e`, or 0 for a root or a stale handle. */
AVER_SCENE_ABI int32_t aver_scene_parent(int32_t e);
/* First child of `e`, or 0. Walk siblings with aver_scene_next_sibling to enumerate all children. */
AVER_SCENE_ABI int32_t aver_scene_first_child(int32_t e);
/* Next sibling under the same parent, or 0 when `e` is the last child. */
AVER_SCENE_ABI int32_t aver_scene_next_sibling(int32_t e);
/* Number of immediate children of `e`. */
AVER_SCENE_ABI int32_t aver_scene_child_count(int32_t e);

/* ---- enumeration ---- */
/* Number of LIVE entities. Indices into aver_scene_at SHIFT on the next flush. */
AVER_SCENE_ABI int32_t aver_scene_count(void);
/* The live entity at dense `index` in [0, aver_scene_count()), or 0 out of range. */
AVER_SCENE_ABI int32_t aver_scene_at(int32_t index);

/* ---- content resolution at bind time ---- */
/* Resolve a material NAME to a stable, positive i32 token within this process; 0 for an empty name.
 * `name0` is the content-pack id (0 == default pack). An opaque token here; the render side maps it. */
AVER_SCENE_ABI int32_t aver_scene_material(int32_t name0, const char* name);
/* The NAME a token was interned under, or "" for 0 or an unknown token. Never NULL. The pointer is
 * owned by the world and must NOT be freed.
 *
 * THE INVERSE EXISTS BECAUSE A TOKEN MUST NEVER BE PERSISTED. Its value is `table.size() + 1` at the
 * moment the name was first seen IN THIS PROCESS, so it depends on startup order and means nothing
 * in another run -- see docs/CHUNKS.md 5.1. Anything writing a surface out (the editor's level save,
 * and the region writer after it) has to turn the token back into the name, and until this function
 * existed it could not: the editor's saveLevel simply dropped every material it had loaded. */
AVER_SCENE_ABI const char* aver_scene_material_name(int32_t token);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_SCENE_ABI_H */
