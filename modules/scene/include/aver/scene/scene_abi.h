#ifndef AVER_SCENE_ABI_H
#define AVER_SCENE_ABI_H

/* Scene C ABI — the stable surface the C# scripting layer binds to via P/Invoke.
 *
 * Deliberately plain C, in the same idiom as pbr_abi.h and voxi_abi.h: only int32_t / int64_t /
 * float / const char* cross the boundary, so the same header works for C, C++, C# DllImport and any
 * other FFI. Vectors are returned through a float* OUT-PARAM, never as a small struct by value —
 * the by-value-small-struct return convention differs between compilers and marshallers, and
 * getting it wrong corrupts the return register rather than failing to link.
 *
 * TWO rules this header is under that pbr_abi.h is not:
 *
 * 1. NO GAMEPLAY VOCABULARY. Not `actor`, not `pawn`, not `spawn`, not `possess`, not `play`. This
 *    module stores entities and components; what a component MEANS is the framework's business.
 *    The rule is enforced on a link line — Aver.Scene does not link Aver.Framework — but it is
 *    stated here because a header is where the temptation arrives first.
 *
 * 2. NO FUNCTION POINTERS, no void*, no structs and no enums. Everything here is marshalled by C#,
 *    and a blittable surface with no callbacks is one the bindings cannot get subtly wrong.
 *    Dispatch tables live in the framework's framework_hooks.h, which no managed code ever reads.
 *
 * Error convention, matching pbr_abi.h: setters return 1 on success, 0 if the request was rejected
 * (stale handle, bad field, or a value out of range). Getters return the current value, or a
 * documented neutral value for a stale handle.
 */

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

/* ABI version, as (major << 16) | minor.
 *
 * MAJOR changes when an existing entry point changes shape or meaning; a managed binding compiled
 * against a different major must refuse to run. MINOR changes when entry points are only ADDED, so
 * an older binding still works against a newer engine and checks `minor >= what it needs`.
 *
 * This is a THIRD version boundary, distinct from the two scripting_abi.h already documents (the
 * host<->bridge contract, and the Aver.Scripting assembly version). It is separate because this DLL
 * can be replaced without either of those moving, and a silent mismatch here is a wrong pointer
 * rather than a missing method. */
#define AVER_SCENE_ABI_VERSION_MAJOR 1
#define AVER_SCENE_ABI_VERSION_MINOR 0
#define AVER_SCENE_ABI_VERSION \
    ((AVER_SCENE_ABI_VERSION_MAJOR << 16) | AVER_SCENE_ABI_VERSION_MINOR)

/* Returns AVER_SCENE_ABI_VERSION as the DLL was BUILT with, which is the whole point: the caller
 * compares it against the constant IT was built with, and the two disagree exactly when a stale
 * binary is present. */
AVER_SCENE_ABI int32_t aver_scene_abi_version(void);

/* ================================================================================================
 * FIELD KINDS — pinned one-for-one to aver::scene::FieldKind (Fields.hpp). A field id resolved from
 * a qualified name carries its kind; aver_scene_field_kind returns one of these. The typed accessor
 * families below each own exactly one (or, for the vector family, one shape) of these kinds, and a
 * set through the wrong family is REJECTED — this is why there is no generic "set bytes".
 * ============================================================================================== */
#define AVER_SCENE_KIND_F32    0
#define AVER_SCENE_KIND_VEC3   1
#define AVER_SCENE_KIND_QUAT   2
#define AVER_SCENE_KIND_I32    3
#define AVER_SCENE_KIND_BOOL   4
#define AVER_SCENE_KIND_I64    5
#define AVER_SCENE_KIND_ENTITY 6
#define AVER_SCENE_KIND_STRING 7
#define AVER_SCENE_KIND_MAT4   8

/* ================================================================================================
 * FIXED COMPONENT IDS — pinned to aver::scene::kComponent* (Components.hpp). The eight built-ins are
 * registered before any caller runs, so a managed binding may assert these are stable. Script-declared
 * components take ids above AVER_SCENE_COMP_CAMERA.
 * ============================================================================================== */
#define AVER_SCENE_COMP_LOCAL         1
#define AVER_SCENE_COMP_WORLD         2
#define AVER_SCENE_COMP_HIERARCHY     3
#define AVER_SCENE_COMP_NAME          4
#define AVER_SCENE_COMP_TAGS          5
#define AVER_SCENE_COMP_MESH_RENDERER 6
#define AVER_SCENE_COMP_LIGHT         7
#define AVER_SCENE_COMP_CAMERA        8

/* ================================================================================================
 * THE ONE WORLD. Every entry point below acts on a single process-global World (aver::scene::World,
 * one-per-process by design), created lazily on first use. No world handle crosses the ABI for the
 * same reason the C++ side has none: inventing a parameter for a thing that can only have one instance
 * costs every call site and buys nothing until multi-world exists, at which point it is additive.
 *
 * STRING MARSHALLING. Every `const char*` IN is treated as UTF-8 bytes, and every `const char*` OUT
 * is a UTF-8, NUL-terminated pointer the caller decodes and must NOT free (it is owned by the world).
 * The engine's name blob is byte-transparent, so the C side neither validates nor transcodes; a
 * managed binding must marshal as UTF-8 (LPUTF8Str) to round-trip a non-ASCII name.
 *
 * ENTITIES cross as int32_t: a live entity has bit 31 clear so it is always positive, and 0 is the
 * only invalid value. Getters return a documented neutral for a stale handle; setters return 0.
 * ============================================================================================== */

/* ---- field resolution ---- */
/* Resolve "Component.field" (e.g. "CLocal.position") to a dense field id; 0 when unknown. */
AVER_SCENE_ABI int32_t aver_scene_field(const char* qualifiedName);
/* The AVER_SCENE_KIND_* of a field id, or 0 (== F32) for an unknown id. */
AVER_SCENE_ABI int32_t aver_scene_field_kind(int32_t f);
/* Floats-per-value for a float kind, 0 for the rest — what a vector accessor sizes its buffer from. */
AVER_SCENE_ABI int32_t aver_scene_field_arity(int32_t f);

/* ---- typed get/set, one family per kind; the kind check is the whole point of a typed field id ----
 * f32   : AVER_SCENE_KIND_F32.
 * vec   : any float kind (F32=1, VEC3=3, QUAT=4, MAT4=16 floats); the buffer must hold `arity` floats.
 * i32   : AVER_SCENE_KIND_I32 or _BOOL (a bool rides an i32).
 * i64   : AVER_SCENE_KIND_I64 (the ObjectId / mesh family).
 * ref   : AVER_SCENE_KIND_ENTITY (an entity reference is its OWN kind, not an int32 in disguise).
 * str   : AVER_SCENE_KIND_STRING.
 * A get on a stale handle or wrong-kind field returns the neutral value (0 / 0.0f / "" / no write);
 * a set returns 0 and writes nothing. Writing any CLocal field bumps the transform revision so the
 * world-matrix pass sees it. */
AVER_SCENE_ABI float   aver_scene_get_f32(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_f32(int32_t e, int32_t f, float v);
AVER_SCENE_ABI int32_t aver_scene_get_vec(int32_t e, int32_t f, float* outv);
AVER_SCENE_ABI int32_t aver_scene_set_vec(int32_t e, int32_t f, const float* v);
AVER_SCENE_ABI int32_t aver_scene_get_i32(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_i32(int32_t e, int32_t f, int32_t v);
AVER_SCENE_ABI int64_t aver_scene_get_i64(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_i64(int32_t e, int32_t f, int64_t v);
AVER_SCENE_ABI int32_t aver_scene_get_ref(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_ref(int32_t e, int32_t f, int32_t v);
AVER_SCENE_ABI const char* aver_scene_get_str(int32_t e, int32_t f);   /* "" for stale/wrong-kind */
AVER_SCENE_ABI int32_t     aver_scene_set_str(int32_t e, int32_t f, const char* v);

/* ---- entity lifetime + hierarchy ---- */
/* Create an unnamed entity (CName/CLocal/CWorld/CHierarchy attached at birth). 0 if none could be made. */
AVER_SCENE_ABI int32_t aver_scene_create(void);
/* Deferred to the next flush and takes the whole subtree; the handle stays valid for the rest of the frame. */
AVER_SCENE_ABI int32_t aver_scene_destroy(int32_t e);
/* 1 if the handle addresses a live entity, else 0 (rejects 0, a stale generation, and a freed slot). */
AVER_SCENE_ABI int32_t aver_scene_valid(int32_t e);
/* Attach a component pool's storage to `e`; returns 1 (idempotent), 0 if the type or handle is bad. */
AVER_SCENE_ABI int32_t aver_scene_add_component(int32_t e, int32_t component);
/* Reparent; parent 0 makes `child` a root. Refuses a cycle, a self-parent, or a doomed parent. */
AVER_SCENE_ABI int32_t aver_scene_set_parent(int32_t child, int32_t parent);

/* ---- persisted identity (CName.objectId, reached here — NOT as a component field) ---- */
/* objectId is u64 in the world; it crosses as int64_t (identity, not magnitude, so the reinterpret is fine). */
AVER_SCENE_ABI int64_t aver_scene_object_id(int32_t e);               /* 0 for a stale handle */
AVER_SCENE_ABI int32_t aver_scene_set_object_id(int32_t e, int64_t objectId);
AVER_SCENE_ABI const char* aver_scene_name(int32_t e);               /* "" for a stale handle */
AVER_SCENE_ABI int32_t     aver_scene_set_name(int32_t e, const char* name);

/* ---- content resolution at bind time ---- */
/* Resolve a material NAME to a stable, positive i32 handle within this process; 0 for an empty name.
 * `name0` is the content-pack id (0 == default pack). Materials belong to Aver.Render.PBR, which this
 * module MUST NOT link — so this is a pure per-name intern into a table local to the scene DLL, NEVER
 * a call into the PBR material library. The render side is what maps this i32 back to a real material;
 * to Aver.Scene it is an opaque token, exactly like CMeshRenderer.material. */
AVER_SCENE_ABI int32_t aver_scene_material(int32_t name0, const char* name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_SCENE_ABI_H */
