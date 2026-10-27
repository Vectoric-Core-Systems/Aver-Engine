#pragma once
// Aver.Physics: per-body contact monitoring and Areas (G2, Godot's contact_monitor and Area3D). Docs: docs/physics/GODOT_AREAS.md.
//
// OPT-IN, ADDITIVE. A world that never calls anything here steps exactly as before, and the existing global queues
// (aver_phys_contact_* and aver_phys_overlap_*) keep their meaning. Both features are configuration, not simulation state:
// physics snapshots do not capture them. A restore re-seeds the contact monitors silently and leaves areas to correct themselves
// on the next step (docs/physics/GODOT_AREAS.md says how).
// Units: cm, cm/s^2 for gravity, kg*cm/s^2 for the wind force (aver_phys_body_add_force's unit), +X forward, +Y right, +Z up, left-handed.
// Returns 1/0 with aver_phys_last_error unless stated. Setters are between-step calls.
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

#define AVER_PHYS_AREAS_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_areas_abi_version(void);

/* ---- contact monitor (Godot contact_monitor + max_contacts_reported) ----
   A monitored body keeps the set of solid bodies it touches and reports BEGIN (body_entered) and END (body_exited) once per body PAIR,
   however many sub-shapes touch. Sensors and areas are not contacts. A body that FALLS ASLEEP does not end its touches (Jolt drops a
   sleeping body's contacts; the monitor tells that from a real separation and waits for the wake). A touch lost while a body sleeps ends
   only if it is not re-established within 3 steps of the wake. Removing a body ends its pairs (the END arrives with the next aver_phys_step
   call's events). Characters (CharacterVirtual) are not contacts. Soft bodies are tracked like any body.
   Turning a monitor on for a body already touching something reports BEGIN for it on the next step.
   At most maxContacts partners are tracked per body (0 tracks nothing); a further partner is promoted, with a BEGIN, when a slot frees. */
AVER_PHYS_API int32_t aver_phys_body_set_contact_monitor(int32_t body, int32_t on, int32_t maxContacts);     /* maxContacts 0..1024 */
AVER_PHYS_API int32_t aver_phys_body_contact_monitor(int32_t body, int32_t* outMaxContacts);                   /* 1 if monitored */
AVER_PHYS_API int32_t aver_phys_body_contact_count(int32_t body);                                              /* tracked partners now; 0 if unmonitored */
/* The index-th tracked partner (ascending handle). The point is where the pair first touched (cm, world) and the normal points from `body` to
   the partner; neither is refreshed while the touch lasts. outPoint and outNormal are float[3], either may be null. */
AVER_PHYS_API int32_t aver_phys_body_contact_get(int32_t body, int32_t index, int32_t* outOther, float* outPoint, float* outNormal);

/* BEGIN and END records, one per monitored side, cleared at the top of each aver_phys_step call and gathering every fixed step in it.
   A pair of two monitored bodies yields one record for each. outBegan is 1 for BEGIN, 0 for END; the point and normal are as above. */
AVER_PHYS_API int32_t aver_phys_contact_event_count(void);
AVER_PHYS_API int32_t aver_phys_contact_event_get(int32_t index, int32_t* outBody, int32_t* outOther, int32_t* outBegan,
                                                  float* outPoint, float* outNormal);

/* ---- areas (Godot Area3D) ----
   An area is a SENSOR body (aver_phys_add_sensor_*, or aver_phys_body_set_sensor) registered with aver_phys_area_create; its handle is the
   body's. Detection is the area's own query: at the end of every fixed step the sensor's shape is tested against the world, so STATIC and
   SLEEPING bodies are found (a Jolt static sensor only sees awake movers; making it kinematic with CollideKinematicVsNonDynamic would need
   an always-awake sensor and costs far more). One enter and one exit per (area, body) however many sub-shapes overlap. The 16-layer
   matrix is not consulted; the rule is the area's own mask against the other's layer bits: (area.mask & other.layer) != 0, where a body's
   layer bits are its Godot layer (aver_phys_body_set_collision_filter) or else the single bit of its user layer. An area is detected by
   another area when it is `monitorable` and its own layer (aver_phys_area_set_collision, default the bit of its user layer) is in that
   area's mask. Plain sensors, soft bodies and character inner bodies are never detected.
   Bodies that are not dynamic only trigger events; the overrides below act on dynamic bodies. */
#define AVER_PHYS_AREA_OVERRIDE_DISABLED         0   /* this area does not touch the property */
#define AVER_PHYS_AREA_OVERRIDE_COMBINE          1   /* adds to what higher-priority areas gave, keeps going */
#define AVER_PHYS_AREA_OVERRIDE_COMBINE_REPLACE  2   /* adds, then stops: lower-priority areas and the world are ignored */
#define AVER_PHYS_AREA_OVERRIDE_REPLACE          3   /* replaces what was gathered, stops */
#define AVER_PHYS_AREA_OVERRIDE_REPLACE_COMBINE  4   /* replaces what was gathered, keeps going */

AVER_PHYS_API int32_t aver_phys_area_create(int32_t body);      /* the body must be a sensor; 1 if it is (or already was) an area */
AVER_PHYS_API int32_t aver_phys_area_destroy(int32_t area);     /* back to a plain sensor; bodies in it lose its overrides next step */
AVER_PHYS_API int32_t aver_phys_area_exists(int32_t area);
AVER_PHYS_API int32_t aver_phys_area_count(void);

/* monitoring: this area reports who is inside (events and the overlap list); turning it off ends what was reported, on the next step.
   monitorable: other areas can detect this one. Both default to 1. The overrides below work with monitoring off. */
AVER_PHYS_API int32_t aver_phys_area_set_flags(int32_t area, int32_t monitoring, int32_t monitorable);
AVER_PHYS_API int32_t aver_phys_area_flags(int32_t area, int32_t* outMonitoring, int32_t* outMonitorable);
/* The area's own layer bits (as a target) and the mask it scans with. Defaults: the bit of the sensor's user layer, and every bit. */
AVER_PHYS_API int32_t aver_phys_area_set_collision(int32_t area, uint32_t layerBits, uint32_t maskBits);
AVER_PHYS_API int32_t aver_phys_area_collision(int32_t area, uint32_t* outLayerBits, uint32_t* outMaskBits);

/* Overrides. Where bodies sit in several areas, the areas apply from the HIGHEST priority down (ties: lowest handle first) and the world's
   gravity, or the body's own damping, comes last unless a COMBINE_REPLACE or REPLACE stopped the walk. A body's own gravity factor still
   scales the result. Overrides are re-evaluated every fixed step from the bodies' present positions, so they follow a moving area.
   Sleeping bodies are not pushed; one is woken when it first falls under an area or the area's effect on it changes. */
AVER_PHYS_API int32_t aver_phys_area_set_priority(int32_t area, int32_t priority);
/* Gravity in cm/s^2: `magnitude` times the world-space direction (dx,dy,dz, normalised here). With point gravity on, the direction is ignored. */
AVER_PHYS_API int32_t aver_phys_area_set_gravity(int32_t area, int32_t mode, float magnitudeCmS2, float dx, float dy, float dz);
/* Point gravity: toward the point (cx,cy,cz), given in the AREA's local space (cm, rotated with the sensor). With unitDistanceCm > 0 the
   strength is magnitude * (unit / distance)^2, i.e. `magnitude` at the unit distance; with 0 it is constant. At the centre it is zero. */
AVER_PHYS_API int32_t aver_phys_area_set_gravity_point(int32_t area, int32_t on, float cx, float cy, float cz, float unitDistanceCm);
/* Damping inside the area (per second, as aver_phys_body_set_damping). The body's own damping stands in for the world's default. While a
   body is overridden its Jolt damping is rewritten each step; it is restored when it leaves. */
AVER_PHYS_API int32_t aver_phys_area_set_damping(int32_t area, int32_t linearMode, float linear, int32_t angularMode, float angular);
/* A wind-like force on dynamic bodies inside: forceMagnitude (kg*cm/s^2) along the world direction (dx,dy,dz). attenuation > 0 weakens it
   beyond unitDistanceCm from the area's origin as (unit/distance)^attenuation; 0 is uniform. Independent of the space-override modes. */
AVER_PHYS_API int32_t aver_phys_area_set_wind(int32_t area, float forceMagnitude, float dx, float dy, float dz,
                                              float attenuation, float unitDistanceCm);

/* Who is inside, as of the last fixed step: every body and every monitorable area the monitoring area reported (ascending handle).
   Empty while monitoring is off. outIsArea is 1 when the entry is another area. */
AVER_PHYS_API int32_t aver_phys_area_overlap_count(int32_t area);
AVER_PHYS_API int32_t aver_phys_area_overlap_get(int32_t area, int32_t index, int32_t* outOther, int32_t* outIsArea);

/* Enter and exit records for monitoring areas, cleared at the top of each aver_phys_step call. outEntered is 1 for an enter, 0 for an exit. */
AVER_PHYS_API int32_t aver_phys_area_event_count(void);
AVER_PHYS_API int32_t aver_phys_area_event_get(int32_t index, int32_t* outArea, int32_t* outOther, int32_t* outEntered, int32_t* outIsArea);

/* What the areas and the world would give a dynamic body right now (before its gravity factor): gravity in cm/s^2 (float[3]) and the damping
   its Jolt body would be set to. Computed fresh from positions, for scripts that need Godot's total_gravity / total damping. */
AVER_PHYS_API int32_t aver_phys_body_area_gravity(int32_t body, float* outGravityCmS2);
AVER_PHYS_API int32_t aver_phys_body_area_damping(int32_t body, float* outLinear, float* outAngular);

#ifdef __cplusplus
}
#endif
