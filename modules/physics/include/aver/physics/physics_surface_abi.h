#pragma once
// Aver.Physics: physical surfaces, what a collision shape is made of and what a hit, contact, character ground or wheel reports.
// A surface is a name hashed to a 32-bit id: Aver.Framework.Surface.IdOf (FNV-1a over UTF-8, never 0), the same number the engine's
// graph rows hand scripts. Names are case-sensitive; 0 = no surface. Friction and restitution are shared by every shape using a surface,
// so aver_phys_surface_tune edits all of them. Building a shape with a surface id interns it.
// Contacts combine each side's leaf material (friction sqrt(f1 f2), restitution max); a surface sampler (physics_water_abi.h) runs after and wins.
// A BODY LABEL (aver_phys_body_set_surface) names a hit but does NOT change contact friction unless copyPhysics = 1 copies the numbers once.
// Which surface a hit names: the leaf material at that sub-shape if it is a surface, else the body label, else 0.
// SUB-SHAPE IDS are raw Jolt values, valid only while the shape is unchanged: never invent one, pass back what a hit or contact gave you.
// Units: cm, +X forward, +Y right, +Z up, left-handed. Registry writes are between-step calls on the stepping thread.
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
   FRICTION and RESTITUTION are the numbers the resolved surface carries (-1 = defers to the body or no surface); not necessarily
   what a contact applied. */
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

/* CHILD is -1 when the shape is not a compound; TRIANGLE is -1 unless the mesh has per-triangle user data; CELL_X / CELL_Y are -1 unless a heightfield. */
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

/* ---- contact detail blocks (aver_phys_contact_detail) ----
   CLOSING_SPEED_CMS: relative speed along the normal at the contact point when the contact began (before the solver); positive = approaching.
   FRICTION and RESTITUTION are the values the contact applies, after the leaf materials and any surface sampler. */
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

/* FNV-1a 32 over the UTF-8 bytes; 0 for NULL or empty; a zero hash becomes 1. Golden values: "Ice" -714267872, "Grass" 1479220765,
   "Rubber" 1606840731, "Wood" 81868168. */
AVER_PHYS_API int32_t aver_phys_surface_id(const char* utf8Name);

/* Upserts a named surface and returns its id, or 0 (refused, logged) for a NULL or empty name, a non-finite number or a hash collision
   with a different name. friction and restitution below 0 mean "defer to the body"; both clamp to 10. `tag` is an opaque number for games. */
AVER_PHYS_API int32_t aver_phys_surface_register(const char* utf8Name, float friction, float restitution, int32_t tag);

/* Changes the numbers of a surface that is registered or interned by a shape. Live: every body on it takes the new value at its next
   contact evaluation. 0 (BadHandle) for an id nothing has used. */
AVER_PHYS_API int32_t aver_phys_surface_tune(int32_t surface, float friction, float restitution, int32_t tag);

/* 1 when the id is known. An id interned only by a shape has an empty name and the numbers -1, -1, 0. The name is UTF-8,
   NUL-terminated and truncated to nameCap bytes. */
AVER_PHYS_API int32_t aver_phys_surface_info(int32_t surface, float* outFriction, float* outRestitution, int32_t* outTag,
                                             uint8_t* outName, int32_t nameCap);

/* Registered surfaces (those with a name), and the id at a dense index in registration order (0 out of range). */
AVER_PHYS_API int32_t aver_phys_surface_count(void);
AVER_PHYS_API int32_t aver_phys_surface_at(int32_t index);

/* Forgets every name and returns every surface's numbers to unset. Shapes holding the materials stay alive and defer to their bodies. */
AVER_PHYS_API void    aver_phys_surface_reset(void);

/* ---- the body label ---- */

/* Labels a body (or character) with `surface`; 0 clears it. copyPhysics = 1 also copies the surface's registered friction and restitution
   (when >= 0) into the body. Returns 0 for a dead handle. */
AVER_PHYS_API int32_t aver_phys_body_set_surface(int32_t body, int32_t surface, int32_t copyPhysics);
/* The label; 0 = none, -1 for a dead handle. */
AVER_PHYS_API int32_t aver_phys_body_surface(int32_t body);

/* ---- what was hit ---- */

/* The same cast as aver_phys_raycast_ex (sensors are skipped unless asked); the blocks are written only on a hit. nF / nI are capacities. */
AVER_PHYS_API int32_t aver_phys_raycast_detail(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm,
                                               uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                               float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* The same blocks for any (body, sub-shape) pair from a hit, a contact or the ground; POINT, NORMAL and DISTANCE_CM are zero. */
AVER_PHYS_API int32_t aver_phys_subshape_info(int32_t body, int32_t subShape, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* Contact i of the last aver_phys_step, index-aligned with aver_phys_contact_get and cleared with it. */
AVER_PHYS_API int32_t aver_phys_contact_detail(int32_t index, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* What a character stands on: the hit blocks from Jolt's ground fields. 1 when there is a ground body, 0 in the air or on another
   character (not an error). */
AVER_PHYS_API int32_t aver_phys_character_ground_detail(int32_t ch, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* What a finished vehicle's wheel is touching: the hit blocks from the wheel's contact. 1 with contact, 0 in the air. */
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_surface(int32_t v, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI);

#ifdef __cplusplus
}
#endif
