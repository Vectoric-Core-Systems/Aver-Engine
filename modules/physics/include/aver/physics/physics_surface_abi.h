#pragma once
// Aver.Physics -- PHYSICAL SURFACES: what a collision shape is made of, and what a hit, a contact, the ground under a
// character or a wheel's tyre reports about it.
//
// A SURFACE IS A NAME HASHED TO A 32-BIT ID, the id the engine already uses: Aver.Framework.Surface.IdOf (FNV-1a over
// the UTF-8 name, never 0) and the graph rows an_surfaceid / an_landscapesurface hand scripts that same number. The
// registry here keys on exactly that hash, so a raycast on a mesh, a contact on a crate and Landscape.Sample on the
// terrain all answer in one id space. Names are case-sensitive; 0 is "no surface".
//
// MATERIALS. Shapes carry a surface where Jolt has a slot: one per convex leaf (the `surface` argument of the builders
// in physics_shape_builder_abi.h), a palette of up to 32 per mesh, up to 256 per heightfield. A surface's friction and
// restitution are shared by every shape that uses it, so aver_phys_surface_tune edits all of them. Building a shape
// with a surface id interns it: ids never need registering first. Registering only adds a name and numbers.
//
// WHAT A CONTACT USES. Friction and restitution are combined from the leaf material of each side when it has a number
// (>= 0), else the body's own, by Jolt's rules (friction sqrt(f1 f2), restitution max(r1 r2)). Until any surface has
// ever been interned the same arithmetic runs on the body values alone, so a project that never uses a surface
// behaves bit for bit as before. A surface sampler (physics_water_abi.h) still runs after the combine and wins on its
// terrain body. A BODY LABEL (aver_phys_body_set_surface) is identity for queries and does NOT change contact
// friction; pass copyPhysics = 1 to copy the registered numbers into the body once.
//
// THE LABEL lives in bits 32 to 63 of the body's user data; the entity id (aver_phys_set_entity) keeps the low 32 and
// every reader truncates to int32, so the two coexist. Static-to-movable rebuilds and vehicle chassis swaps keep it.
//
// WHICH SURFACE A HIT NAMES, in order: the leaf material at that sub-shape if it is a surface, else the body label,
// else 0. A character's ground is the body and sub-shape Jolt remembers under its feet; soft bodies have no leaf materials.
//
// SUB-SHAPE IDS ARE RAW JOLT VALUES (HITI_SUB_SHAPE): valid only while the shape is unchanged. A mutable-compound edit,
// aver_phys_body_set_shape / set_scale, a heightfield edit or a released shape invalidates every id taken before;
// callers re-query. HITI_CHILD and HITI_CELL_* are the stable-per-shape identities. Never invent an id: pass back what a
// hit, a contact or the ground gave you.
//
// UNITS AND AXES ARE THE ENGINE'S: centimetres, +X forward, +Y right, +Z up, left-handed. Failures return 0 (or -1 where
// 0 is a real answer) and set aver_phys_last_error. Registry writes and tune/set_surface are between-step calls on the
// thread that steps: a tune during aver_phys_step from another thread is a data race.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_PHYS_SURFACE_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_surface_abi_version(void);                                   /* 1 */

/* ---- hit detail blocks (aver_phys_raycast_detail, aver_phys_subshape_info, character ground, wheel surface) ----
   Float block. FRICTION and RESTITUTION are the numbers the resolved surface CARRIES (-1 when it defers to the body or
   there is no surface); they are not necessarily what a contact applied (a body label does not change contact friction). */
#define AVER_PHYS_HITF_POINT_X        0
#define AVER_PHYS_HITF_POINT_Y        1
#define AVER_PHYS_HITF_POINT_Z        2
#define AVER_PHYS_HITF_NORMAL_X       3
#define AVER_PHYS_HITF_NORMAL_Y       4
#define AVER_PHYS_HITF_NORMAL_Z       5
#define AVER_PHYS_HITF_DISTANCE_CM    6
#define AVER_PHYS_HITF_FRICTION       7
#define AVER_PHYS_HITF_RESTITUTION    8
#define AVER_PHYS_HITF_COUNT          9

/* Int block. CHILD is the index of the top-level compound child the sub-shape lies in (-1 when the shape is not a
   compound), CHILD_USER_DATA that child's user data. TRIANGLE is a mesh's per-triangle user data when the mesh was built
   with it (aver_phys_shape_mesh triUserData), else -1. CELL_X / CELL_Y is the heightfield cell in physics order, else -1. */
#define AVER_PHYS_HITI_BODY             0
#define AVER_PHYS_HITI_ENTITY           1
#define AVER_PHYS_HITI_SUB_SHAPE        2
#define AVER_PHYS_HITI_SURFACE          3
#define AVER_PHYS_HITI_TAG              4
#define AVER_PHYS_HITI_CHILD            5
#define AVER_PHYS_HITI_CHILD_USER_DATA  6
#define AVER_PHYS_HITI_TRIANGLE         7
#define AVER_PHYS_HITI_CELL_X           8
#define AVER_PHYS_HITI_CELL_Y           9
#define AVER_PHYS_HITI_COUNT            10

/* ---- contact detail blocks (aver_phys_contact_detail) ---- */
/* CLOSING_SPEED_CMS: the relative speed of the two bodies at the contact point along the contact normal, sampled when
   the contact began (before the solver); positive = approaching. FRICTION and RESTITUTION are the values the contact
   applies, after the leaf materials and any surface sampler. */
#define AVER_PHYS_CONF_CLOSING_SPEED_CMS  0
#define AVER_PHYS_CONF_FRICTION           1
#define AVER_PHYS_CONF_RESTITUTION        2
#define AVER_PHYS_CONF_COUNT              3

#define AVER_PHYS_CONI_SUB_SHAPE_A  0
#define AVER_PHYS_CONI_SUB_SHAPE_B  1
#define AVER_PHYS_CONI_SURFACE_A    2
#define AVER_PHYS_CONI_SURFACE_B    3
#define AVER_PHYS_CONI_COUNT        4

/* ---- identity and registry ---- */

/* FNV-1a 32 over the UTF-8 bytes, 0 for NULL or empty, a zero hash becomes 1. Pure; golden values: "Ice" -714267872,
   "Grass" 1479220765, "Rubber" 1606840731, "Wood" 81868168. */
AVER_PHYS_API int32_t aver_phys_surface_id(const char* utf8Name);

/* Upserts a named surface and returns its id, or 0 (refused, logged) for a NULL or empty name, a non-finite number or a
   hash collision with a different registered name. friction and restitution below 0 mean "defer to the body"; both
   clamp to 10. `tag` is an opaque number for games (a footstep group, an FX class). */
AVER_PHYS_API int32_t aver_phys_surface_register(const char* utf8Name, float friction, float restitution, int32_t tag);

/* Changes the numbers of a surface that is registered or interned by a shape. Live: shapes share the material, so every
   body on it takes the new value at its next contact evaluation. 0 (BadHandle) for an id nothing has used. */
AVER_PHYS_API int32_t aver_phys_surface_tune(int32_t surface, float friction, float restitution, int32_t tag);

/* 1 when the id is known. An id interned only by a shape has an empty name and the numbers -1, -1, 0. Any out-pointer may
   be NULL. The name is UTF-8, NUL-terminated and truncated to nameCap bytes (C# byte[]). */
AVER_PHYS_API int32_t aver_phys_surface_info(int32_t surface, float* outFriction, float* outRestitution, int32_t* outTag,
                                             uint8_t* outName, int32_t nameCap);

/* Registered surfaces (those with a name), and the id at a dense index in registration order (0 out of range). */
AVER_PHYS_API int32_t aver_phys_surface_count(void);
AVER_PHYS_API int32_t aver_phys_surface_at(int32_t index);

/* Forgets every name and returns every surface's numbers to unset (-1, -1, 0). The materials stay alive for the shapes
   holding them, which then defer to their bodies. The registry otherwise outlives aver_phys_shutdown. */
AVER_PHYS_API void    aver_phys_surface_reset(void);

/* ---- the body label ---- */

/* Labels a body (or character) with `surface`; 0 clears it. copyPhysics = 1 also copies the surface's registered friction
   and restitution (when >= 0) into the body through the ordinary setters. Returns 0 for a dead handle. */
AVER_PHYS_API int32_t aver_phys_body_set_surface(int32_t body, int32_t surface, int32_t copyPhysics);
/* The label; 0 = none, -1 for a dead handle (a real id of exactly -1 reads the same: one hash in four billion). */
AVER_PHYS_API int32_t aver_phys_body_surface(int32_t body);

/* ---- what was hit ---- */

/* The same cast as aver_phys_raycast_ex (layerMask, ignoreBody, AVER_PHYS_QUERY_* flags in physics_queries_abi.h: sensors
   are skipped unless asked), with the detail blocks above. Returns the hit body's handle or 0 for a miss; the blocks are
   written only on a hit. nF / nI are the capacities of outF / outI (entries beyond a block's COUNT are not touched). */
AVER_PHYS_API int32_t aver_phys_raycast_detail(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm,
                                               uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                               float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* The same blocks for any (body, sub-shape) pair from a hit, a contact or the ground; POINT, NORMAL and DISTANCE_CM are
   zero. 1 on success. */
AVER_PHYS_API int32_t aver_phys_subshape_info(int32_t body, int32_t subShape, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* Contact i of the last aver_phys_step, index-aligned with aver_phys_contact_get and cleared with it. */
AVER_PHYS_API int32_t aver_phys_contact_detail(int32_t index, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* What a character stands on (or touches, when not supported): the hit blocks from Jolt's ground fields, POINT the ground
   position, NORMAL the ground normal. 1 when there is a ground body, 0 in the air or on another character (not an
   error). */
AVER_PHYS_API int32_t aver_phys_character_ground_detail(int32_t ch, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* What a finished vehicle's wheel is touching: the hit blocks from the wheel's contact. 1 with contact, 0 in the air. */
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_surface(int32_t v, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI);

#ifdef __cplusplus
}
#endif
