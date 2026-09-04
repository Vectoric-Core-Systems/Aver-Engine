#pragma once
// Aver.Physics — the plain-C ABI. Setters return 1/0 and 0 is always an invalid handle.
// Units and axes are the engine's, never Jolt's: centimetres, +X forward, +Y right, +Z up,
// left-handed. No Jolt type crosses this boundary.
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

// ---- World ---------------------------------------------------------------------------------------

// Start the simulation. Idempotent: a second call while running is a no-op that returns 1.
AVER_PHYS_API int32_t aver_phys_init(void);
// Destroy every body and character, then the world.
AVER_PHYS_API void    aver_phys_shutdown(void);
// 1 while a world exists.
AVER_PHYS_API int32_t aver_phys_ready(void);

// Gravity in cm/s^2, engine axes. Defaults to (0, 0, -980) -- one g, straight down +Z-up.
AVER_PHYS_API void aver_phys_set_gravity(float x, float y, float z);

// Advance by `dt` seconds of real time, in fixed steps. Leftover time carries; a long stall is
// clamped. Returns how many fixed steps actually ran.
AVER_PHYS_API int32_t aver_phys_step(float dt);

// The fixed step, in seconds (1/60 unless set).
AVER_PHYS_API float   aver_phys_fixed_step(void);
// Change the fixed step. Set it before any bodies exist.
AVER_PHYS_API int32_t aver_phys_set_fixed_step(float seconds);

// ---- Bodies --------------------------------------------------------------------------------------
// Shapes are described in the engine's units: half-extents and radii in centimetres.

// A box that never moves -- floors, walls, static level geometry.
AVER_PHYS_API int32_t aver_phys_add_static_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);

// A box that falls and collides. `massKg` <= 0 asks Jolt to derive mass from the shape's volume.
AVER_PHYS_API int32_t aver_phys_add_dynamic_box(float cx, float cy, float cz,
                                                float hx, float hy, float hz, float massKg);

// A sphere that falls and collides.
AVER_PHYS_API int32_t aver_phys_add_dynamic_sphere(float cx, float cy, float cz,
                                                   float radius, float massKg);

// Remove a body from the world and destroy it.
AVER_PHYS_API int32_t aver_phys_remove_body(int32_t body);

// Read a body's transform. `outXyz` / `outQuat` are caller-owned float[3] / float[4]. Returns 0 for
// a dead handle, leaving the outputs untouched.
AVER_PHYS_API int32_t aver_phys_body_position(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_rotation(int32_t body, float* outQuat);
AVER_PHYS_API int32_t aver_phys_body_velocity(int32_t body, float* outXyz);

// Teleport a body and wake it.
AVER_PHYS_API int32_t aver_phys_body_set_position(int32_t body, float x, float y, float z);
// Set a body's linear velocity, cm/s.
AVER_PHYS_API int32_t aver_phys_body_set_velocity(int32_t body, float x, float y, float z);

// How many bodies are live.
AVER_PHYS_API int32_t aver_phys_body_count(void);

/* The body handle at a dense index, or 0. Pairs with aver_phys_body_count, which could always say
   HOW MANY bodies exist while nothing could ask WHICH -- so no caller could iterate them.
   Indices shift on add/remove, like scene::World::at: for a walk inside one frame, not a handle. */
AVER_PHYS_API int32_t aver_phys_body_at(int32_t index);

/* A body's WORLD-SPACE bounding box, for a collider overlay. An AABB rather than the shape itself:
   for the axis-aligned boxes most level collision is, the box IS the shape; for a sphere, capsule
   or mesh it is an honest bound. Drawing every Jolt shape type would need an ABI that can describe
   them all. Returns 0 on an invalid handle or a null pointer. */
AVER_PHYS_API int32_t aver_phys_body_aabb(int32_t body, float* outMin, float* outMax);

// ---- Body dynamics --------------------------------------------------------------------------------
// Everything above lets a caller PLACE a body and read where it ended up. This is the half that lets
// gameplay push one around: forces, impulses, spin, and the material properties that decide how it
// answers. Without them a body can only be teleported or have its velocity overwritten, which is why
// an explosion, a knockback, a rolling ball and a bouncy one were all equally unbuildable.
//
// UNITS ARE THE ENGINE'S THROUGHOUT, and they follow from the two this ABI already fixed: kilograms
// (aver_phys_add_dynamic_box's massKg) and centimetres. So
//
//   force            kg*cm/s^2   -- mass times an acceleration, and gravity is already cm/s^2
//   impulse          kg*cm/s     -- mass times a velocity, and velocity is already cm/s
//   torque           kg*cm^2/s^2 -- a force times a lever arm, so centimetres TWICE
//   angular impulse  kg*cm^2/s
//   angular velocity rad/s       -- radians are dimensionless; no length to convert
//
// A ROTATIONAL QUANTITY IS NOT A DIRECTION. Angular velocity, torque and angular impulse are axial
// vectors: they flip sign between the engine's left-handed axes and Jolt's right-handed ones, exactly
// as this module's quaternion converter has always done. See Convert.hpp's own derivation -- the
// consequence of ignoring it is a body that spins the wrong way with nothing reporting a problem.

// Which motion type a body has. The values match JPH::EMotionType's own declared order, so this enum
// and Jolt's cannot drift apart silently.
//
// KINEMATIC IS THE ONE THAT WAS MISSING, and it is what a moving platform, a lift, a swinging door
// driven by animation and a scripted crane all are: it collides with and pushes dynamic bodies, and
// nothing -- not gravity, not an impulse, not a collision -- pushes it back. Set its velocity (or
// teleport it) and it goes there regardless of what is in the way.
#define AVER_PHYS_MOTION_STATIC    0
#define AVER_PHYS_MOTION_KINEMATIC 1
#define AVER_PHYS_MOTION_DYNAMIC   2

// Change a body's motion type, activating it if it becomes movable. Returns 0 for a dead handle or an
// unrecognised type.
AVER_PHYS_API int32_t aver_phys_body_set_motion_type(int32_t body, int32_t motionType);
// A body's motion type, or -1 for a dead handle. -1 rather than 0 because 0 is STATIC, a real answer.
AVER_PHYS_API int32_t aver_phys_body_motion_type(int32_t body);

// Set a body's orientation, as a quaternion (x, y, z, w) in engine axes. Wakes it, matching
// aver_phys_body_set_position -- the sibling this completes, which has had no rotational twin.
AVER_PHYS_API int32_t aver_phys_body_set_rotation(int32_t body, float x, float y, float z, float w);

// Angular velocity, radians per second about each engine axis.
AVER_PHYS_API int32_t aver_phys_body_angular_velocity(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_set_angular_velocity(int32_t body, float wx, float wy, float wz);

// Add to a body's linear velocity rather than replacing it, cm/s. `set_velocity` is the right call for
// "this is how fast it is going now"; this one is for "and also this much", which is what every
// accumulating effect wants.
AVER_PHYS_API int32_t aver_phys_body_add_velocity(int32_t body, float vx, float vy, float vz);

// ---- Forces and impulses ---------------------------------------------------------------------------
//
// A FORCE LASTS ONE STEP; AN IMPULSE IS INSTANT. Jolt clears accumulated forces and torques at the end
// of every physics step, so a force applied once is applied for one step and then gone -- to push
// something continuously (thrust, wind, a magnet) the caller re-applies it every frame. An impulse
// instead changes velocity immediately by impulse/mass and does not accumulate. A jump, a bullet hit
// and an explosion are impulses; a rocket motor is a force.
//
// All of them wake a sleeping body, because a force that does not is a force that does nothing to the
// exact bodies most likely to be sitting still when it arrives.

// A force through the centre of mass, kg*cm/s^2, for this step only.
AVER_PHYS_API int32_t aver_phys_body_add_force(int32_t body, float fx, float fy, float fz);
// The same, applied at a world-space point -- which also produces torque about the centre of mass.
AVER_PHYS_API int32_t aver_phys_body_add_force_at(int32_t body, float fx, float fy, float fz,
                                                  float px, float py, float pz);
// A torque about the centre of mass, kg*cm^2/s^2, for this step only.
AVER_PHYS_API int32_t aver_phys_body_add_torque(int32_t body, float tx, float ty, float tz);

// An instantaneous impulse through the centre of mass, kg*cm/s: velocity changes by impulse/mass.
AVER_PHYS_API int32_t aver_phys_body_add_impulse(int32_t body, float ix, float iy, float iz);
// The same at a world-space point, so an off-centre hit spins the body as well as moving it.
AVER_PHYS_API int32_t aver_phys_body_add_impulse_at(int32_t body, float ix, float iy, float iz,
                                                    float px, float py, float pz);
// An instantaneous angular impulse, kg*cm^2/s.
AVER_PHYS_API int32_t aver_phys_body_add_angular_impulse(int32_t body, float ax, float ay, float az);

// ---- Material and mass ------------------------------------------------------------------------------
// What a body is MADE of, as far as the simulation is concerned. Every one of these is a live setter:
// unlike mass at creation, they can be changed on a body already in the world.

// How much a body resists sliding. 0 is frictionless ice, 1 is roughly rubber; Jolt's default is 0.2.
// Two touching bodies combine theirs (geometric mean by default), so a frictionless floor still slows
// a high-friction crate somewhat.
AVER_PHYS_API int32_t aver_phys_body_set_friction(int32_t body, float friction);
AVER_PHYS_API int32_t aver_phys_body_friction(int32_t body, float* outFriction);

// How much a body bounces. 0 keeps none of its approach speed, 1 keeps all of it (in theory -- solver
// damping means a real 1.0 still settles). Jolt's default is 0.
AVER_PHYS_API int32_t aver_phys_body_set_restitution(int32_t body, float restitution);
AVER_PHYS_API int32_t aver_phys_body_restitution(int32_t body, float* outRestitution);

// Scales the world gravity for one body: 1 is normal, 0 makes it float, negative makes it fall up.
// A per-body dial rather than a global one, so a balloon and a brick can share a world.
AVER_PHYS_API int32_t aver_phys_body_set_gravity_factor(int32_t body, float factor);
AVER_PHYS_API int32_t aver_phys_body_gravity_factor(int32_t body, float* outFactor);

// Velocity bleed per second, applied to linear and angular motion independently. 0 is a vacuum; small
// positive values are how a body eventually stops instead of sliding forever on a frictionless floor.
AVER_PHYS_API int32_t aver_phys_body_set_damping(int32_t body, float linear, float angular);
AVER_PHYS_API int32_t aver_phys_body_damping(int32_t body, float* outLinear, float* outAngular);

// A body's mass in kilograms. The setter RESCALES the inertia the shape already implies rather than
// replacing it, so a body keeps rotating like its own shape at its new weight.
//
// DYNAMIC BODIES ONLY, and 0 for anything else: a static or kinematic body has infinite mass by
// definition, and Jolt models that with no MotionProperties at all rather than with a large number.
AVER_PHYS_API int32_t aver_phys_body_set_mass(int32_t body, float massKg);
AVER_PHYS_API int32_t aver_phys_body_mass(int32_t body, float* outMassKg);

// ---- Sleeping ---------------------------------------------------------------------------------------
// Jolt puts a body that has stopped moving to sleep so it costs nothing to simulate, and wakes it when
// something touches it. These are for the cases where nothing will: a body the game is about to act on
// through a force, or one whose neighbour was just removed.

// Wake a sleeping body, or put an awake one to sleep. Every force/impulse call above already activates,
// so these are for the cases that are not a force.
AVER_PHYS_API int32_t aver_phys_body_activate(int32_t body);
AVER_PHYS_API int32_t aver_phys_body_deactivate(int32_t body);
// 1 while a body is awake and being simulated, 0 while it sleeps or for a dead handle.
AVER_PHYS_API int32_t aver_phys_body_is_active(int32_t body);

// ---- Character -----------------------------------------------------------------------------------
// A capsule that walks: swept and resolved rather than simulated as a rigid body.

// Create a character capsule. `radius` and `height` in centimetres; `height` is the TOTAL capsule
// height including both caps.
AVER_PHYS_API int32_t aver_phys_character_create(float radius, float height,
                                                 float x, float y, float z);
// Destroy a character.
AVER_PHYS_API int32_t aver_phys_character_destroy(int32_t ch);

// Set the velocity the character WANTS, cm/s, engine axes. The vertical component is managed by the
// simulation unless it is set here.
AVER_PHYS_API int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz);
// Read a character's velocity into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_character_velocity(int32_t ch, float* outXyz);
// Read a character's position into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_character_position(int32_t ch, float* outXyz);
// Teleport a character.
AVER_PHYS_API int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z);

// 1 while standing on ground steep enough to hold.
AVER_PHYS_API int32_t aver_phys_character_grounded(int32_t ch);

// ---- Entity association ---------------------------------------------------------------------------
// A body or character is an opaque handle with no notion of "which scene entity this is" -- this is
// the one seam that ties one back to the other, for anything (Raycast, above all) that must report
// not just THAT something was hit but WHAT. Works on a body OR a character handle: the two families
// are drawn from one counter and never collide, so a single call covers both.

// Stamps `handle` (a body OR a character) with `entity`, a scene entity id. 0 clears it back to
// "unowned". Returns 0 for a dead handle. Backed by Jolt's own per-body user-data field, so it needs
// no side table and nothing to invalidate when the body or character is destroyed.
AVER_PHYS_API int32_t aver_phys_set_entity(int32_t handle, int32_t entity);

// ---- Arbitrary collision geometry ------------------------------------------------------------------
// Raw arrays in the engine's centimetres, laid out xyz,xyz,... -- not a mesh handle.

// A convex hull wrapped around `count` points.
AVER_PHYS_API int32_t aver_phys_add_convex_hull(const float* pointsXyz, int32_t count,
                                                float cx, float cy, float cz,
                                                int32_t dynamic, float massKg);

// A triangle mesh, STATIC ONLY. `indices` is 3 per triangle.
AVER_PHYS_API int32_t aver_phys_add_mesh(const float* verticesXyz, int32_t vertexCount,
                                         const int32_t* indices, int32_t indexCount,
                                         float cx, float cy, float cz);

// A static heightfield: `samples` is a row-major sampleCount x sampleCount grid of heights in
// centimetres, spaced `spacingCm` apart.
// Sample (column x, row y) lands at engine (cx - y*spacingCm, cy + x*spacingCm, cz + height).
AVER_PHYS_API int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount,
                                                float spacingCm,
                                                float cx, float cy, float cz);

// ---- Sensors (triggers) --------------------------------------------------------------------------
// A sensor detects overlap without pushing anything, and reports through the overlap queue below.

// A box-shaped trigger volume.
AVER_PHYS_API int32_t aver_phys_add_sensor_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);
// A sphere-shaped trigger volume.
AVER_PHYS_API int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);

// ---- Buoyancy -----------------------------------------------------------------------------------
// Things float. A plane of water, and the bodies it holds up.
//
// TWO ENTRY POINTS, AND THE GLOBAL ONE IS THE IMPORTANT ONE. Jolt's own buoyancy is per body, per
// step, and an ABI that only offered that would be an ABI nothing ever called: something would still
// have to decide, every frame, which bodies are in the water -- and no scene volume component exists
// to decide it (there are none among the builtin components, and physics sensors have no callers
// above this module). An engine feature that requires the game to write the loop that makes it work
// is the shape this repo keeps shipping and then finding unused, so the plane comes first and the
// per-body override second.
//
// Engine units and axes throughout: centimetres, +Z up. Buoyancy 1.0 is neutral -- the body's own
// density decides whether it rises or sinks -- above 1 floats harder, below 1 sinks.

// Every dynamic body whose centre is below `heightCm` gets buoyancy this step, and keeps getting it
// until the plane is cleared. This is the call that makes water actually hold things up.
//
// `normalUnit` is the surface normal, normally (0,0,1); it is a parameter because a sloped water
// plane is how a river reads. `fluidVelocityCmS` is the current, and is what carries a body
// downstream. Drags are 0..1-ish damping factors applied to linear and angular motion in the fluid.
//
// Returns 1. Passing a non-positive `buoyancy` is how the plane is disabled without forgetting its
// other settings; use aver_phys_clear_water_plane to remove it outright.
AVER_PHYS_API int32_t aver_phys_set_water_plane(float heightCm,
                                                const float normalUnit[3],
                                                float buoyancy, float linearDrag, float angularDrag,
                                                const float fluidVelocityCmS[3]);

// Removes the global plane. Bodies stop being held up on the next step.
AVER_PHYS_API void aver_phys_clear_water_plane(void);

// 1 while a global water plane is set, and writes its height into `outHeightCm` when non-null. The
// host needs this to keep the RENDERED surface and the SIMULATED one at the same height -- two
// separate numbers for that would drift, and the drift would look like broken buoyancy.
AVER_PHYS_API int32_t aver_phys_water_plane(float* outHeightCm);

// Per-body override, for water that is not the global plane: a puddle, a tank, a body of water at a
// different height. Takes precedence over the plane for that body. Returns 0 for a dead handle.
AVER_PHYS_API int32_t aver_phys_set_water_volume(int32_t body,
                                                 const float surfacePosCm[3],
                                                 const float surfaceNormalUnit[3],
                                                 float buoyancy, float linearDrag, float angularDrag,
                                                 const float fluidVelocityCmS[3]);

// Removes a body's override, returning it to the global plane if one is set. Returns 0 for a dead
// handle or a body that had no override.
AVER_PHYS_API int32_t aver_phys_clear_water_volume(int32_t body);

// How many bodies had buoyancy applied on the last step. A DIAGNOSTIC THAT EARNS ITS PLACE: "nothing
// floats" and "nothing is in the water" look identical from outside, and this is what tells them
// apart without a debugger.
AVER_PHYS_API int32_t aver_phys_buoyant_body_count(void);

// ---- Contact and overlap events ------------------------------------------------------------------
// POLLED, not called back. Both queues are cleared at the start of each aver_phys_step.

// How many contacts between two solid bodies began this step.
AVER_PHYS_API int32_t aver_phys_contact_count(void);
// Read one contact. `outPoint` / `outNormal` are caller-owned float[3]. 0 for an out-of-range index.
AVER_PHYS_API int32_t aver_phys_contact_get(int32_t index, int32_t* outBodyA, int32_t* outBodyB,
                                            float* outPoint, float* outNormal);

// How many sensor overlaps started or stopped this step.
AVER_PHYS_API int32_t aver_phys_overlap_count(void);
// Read one overlap. `outEntered` is 1 for an enter, 0 for an exit.
AVER_PHYS_API int32_t aver_phys_overlap_get(int32_t index, int32_t* outSensor, int32_t* outBody,
                                            int32_t* outEntered);

// ---- Soft bodies ------------------------------------------------------------------------------------
// A deformable mesh: particles held together by distance constraints, colliding with the world.
//
// A SOFT BODY IS A BODY. It is drawn from the same handle counter as everything above, so
// aver_phys_remove_body, aver_phys_set_entity and aver_phys_raycast all work on one unchanged --
// there is no parallel family to keep in step.
//
// WHY JOLT AND NOT A SOLVER OF OUR OWN: Jolt 5.6 ships this, it is already vendored and already
// compiled into this build, and it brings the one thing a hand-written cage solver does not get for
// free -- collision against the real world. The engine's own contribution is the seam: engine units
// and axes on this side, Jolt's on the other, and skinning driven from Aver's animation palette.
//
// WHAT IT DOES NOT DO, so it is not discovered later: Jolt soft bodies are purely ELASTIC. There is
// no plastic deformation, no permanent set, no material yield and no break/tear. A dent that stays
// is a different solver (see docs/recon/softbody-solver.md), not a parameter here.

// Build a soft body from a triangle mesh. Positions are engine centimetres, xyz,xyz,...; `indices`
// is 3 per triangle and gives the FACES, from which the edge constraints are generated.
//
// `invMasses` is optional (NULL = every particle at 1). A particle at inverse mass 0 is PINNED and
// is how a flag stays attached to its pole.
//
// `compliance` is inverse stiffness: 0 is inextensible, larger is stretchier. `pressure` inflates a
// closed mesh from within; 0 for cloth.
//
// `damping` is SoftBodyCreationSettings::mLinearDamping (1/s: dv/dt = -damping * v) and `iterations`
// is ::mNumIterations, Jolt's own per-step solver pass count -- both newly reachable through this
// call as of the fluids solver-knobs change; before it, every soft body this engine created got
// Jolt's own un-named defaults with no way for a caller to ask for anything else. DEFAULTED TO
// THOSE EXACT NUMBERS (SoftBodyCreationSettings.h's own 0.1f/5, not a number this engine chose), so
// every call site written before this change compiles and behaves identically without editing one.
// A `damping` below zero or an `iterations` below 1 is clamped by the implementation -- the same
// guard already applied to a negative `compliance` above, and for `iterations` not optional: Jolt
// divides the step by it (SoftBodyMotionProperties.cpp), so 0 there is a divide-by-zero, not merely
// "no iterations".
AVER_PHYS_API int32_t aver_phys_softbody_create(const float* verticesXyz, int32_t vertexCount,
                                                const int32_t* indices, int32_t indexCount,
                                                const float* invMasses,
                                                float cx, float cy, float cz,
                                                float compliance, float pressure,
                                                float damping = 0.1f, int32_t iterations = 5);

// The same, plus SKINNED constraints -- soft body on a skeletal mesh.
//
// Each vertex is tethered to where ordinary bone skinning would have put it, free to move up to
// `maxDistanceCm` away from it and no further. That single number is the whole dial between "this
// is just skinning" (0) and "this is a free-floating cloth that happens to be near a skeleton"
// (large): jiggle, squash and drape all live in between.
//
// `jointIndices` and `jointWeights` are `vertexCount * influences` long, matching the mesh's own
// skinning data. `backStopDistanceCm` keeps a vertex from sinking back through the surface it hangs
// off -- negative disables it.
//
// PASS THE BIND POSE as `verticesXyz`, because that is what the skinning is defined against. The
// body is then driven each frame by aver_phys_softbody_skin.
AVER_PHYS_API int32_t aver_phys_softbody_create_skinned(const float* verticesXyz, int32_t vertexCount,
                                                        const int32_t* indices, int32_t indexCount,
                                                        const float* invMasses,
                                                        const int32_t* jointIndices,
                                                        const float* jointWeights,
                                                        int32_t influences, int32_t jointCount,
                                                        float maxDistanceCm, float backStopDistanceCm,
                                                        float cx, float cy, float cz,
                                                        float compliance);

// Drive a skinned soft body from an animated pose. `jointMatrices` is `jointCount` matrices of 16
// floats, row-major, in the engine's own convention -- exactly what aver::anim::poseToSkinning
// produces, handed over unchanged.
//
// `hardSkin` snaps every particle onto its skinned position instead of constraining toward it: what
// to pass on the first frame, and after a teleport, so the body starts on the character rather than
// flying in from wherever it was.
//
// Call it BEFORE aver_phys_step each frame. Returns 0 for a handle that is not a skinned soft body.
AVER_PHYS_API int32_t aver_phys_softbody_skin(int32_t body, const float* jointMatrices,
                                              int32_t jointCount, int32_t hardSkin);

// How many particles a soft body has, or 0 for any other handle.
AVER_PHYS_API int32_t aver_phys_softbody_vertex_count(int32_t body);

// Read the deformed particle positions into a caller-owned float[maxVertices*3], as engine
// centimetres in WORLD space. Returns how many were written. This is what the renderer draws.
AVER_PHYS_API int32_t aver_phys_softbody_vertices(int32_t body, float* outXyz, int32_t maxVertices);

// FALLBACK for a gap Jolt itself has: a soft body's own update
// (JPH::SoftBodyMotionProperties::DetermineCollidingShapes) broadphase-queries for nearby RIGID
// bodies and asks their SHAPE to push its particles around -- but aver_phys_character_create builds a
// JPH::CharacterVirtual, whose companion inner body is a kinematic Body that never enters that query
// as a hit (confirmed empirically in SoftBodyTest.cpp: a character embedded dead-centre in a settled
// pool for two seconds moves the nearest vertex no more than the pool's own idle jiggle explains).
// The character's own collision against the pool works fine the other way around -- this function
// exists only to make the MISSING direction happen, from the composition root, deliberately.
//
// `centreCm`/`radiusCm` select which of the soft body's particles react -- every one within
// `radiusCm` of `centreCm` (both world-space engine centimetres), typically the player's current
// position and something a little larger than their capsule radius.
//
// `velocityCmPerS` is the velocity (engine cm/s, world-space) those particles are nudged TOWARD, not
// added by: each selected vertex's velocity moves a `strength` fraction of the way from where it is
// to `velocityCmPerS` (0 = no effect, 1 = snap to it outright). This is a BLEND, not `+=`, on purpose
// -- SandboxApp.cpp calls this once every physics step for as long as the player is near the volume,
// and an additive impulse repeated every step with no decay would run away without bound; a blend
// converges toward the target and stays bounded no matter how many consecutive steps call it.
//
// Modifies VELOCITY ONLY, never a vertex's position directly -- SoftBodyVertex.h's own comment is
// explicit that positions are solver-internal ("Modifying the position can lead to missed
// collisions") and velocity is the sanctioned lever for outside code to move a soft body.
//
// Returns how many vertices fell inside the sphere and were nudged, or 0 for a handle that is not a
// soft body, a non-positive radius, or a missing pointer.
AVER_PHYS_API int32_t aver_phys_softbody_apply_impulse(int32_t body, const float* centreCm,
                                                        float radiusCm, const float* velocityCmPerS,
                                                        float strength);

// ---- Queries -------------------------------------------------------------------------------------

// Cast a ray from `o` along `d` for `maxDistCm`. Returns the hit body handle, or 0 for a miss.
// `outPoint` / `outNormal` / `outEntity` are only written on a hit -- check the RETURN VALUE for
// hit/miss, never `*outEntity` alone. `outEntity` is whatever aver_phys_set_entity last stamped the
// hit handle with, or 0 when nothing ever did: that is a REAL hit against something no entity owns
// (a landscape heightfield today), not a miss, and the two are told apart by the return value being
// non-zero either way.
// A live character IS visible to this query: aver_phys_character_create gives it a broadphase body
// for exactly this reason, so a raycast can identify a character the same way it identifies any
// other body, through this same `handle`/`outEntity` pair.
// Jolt broadphase queries are not deterministic between equidistant bodies.
AVER_PHYS_API int32_t aver_phys_raycast(float ox, float oy, float oz,
                                        float dx, float dy, float dz,
                                        float maxDistCm, float* outPoint, float* outNormal,
                                        int32_t* outEntity);

// Every body whose shape overlaps a sphere. Writes up to `maxBodies` handles into `outBodies` and
// returns how many were written.
AVER_PHYS_API int32_t aver_phys_overlap_sphere(float x, float y, float z, float radius,
                                               int32_t* outBodies, int32_t maxBodies);

// Sweep a sphere along a direction and report the first thing it touches. Returns the hit body
// handle, or 0 for a clear sweep.
AVER_PHYS_API int32_t aver_phys_sphere_cast(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            float maxDistCm, float radius,
                                            float* outPoint, float* outNormal);

#ifdef __cplusplus
}
#endif
