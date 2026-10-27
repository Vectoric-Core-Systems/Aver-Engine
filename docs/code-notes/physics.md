# Code notes: physics

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/physics/include/aver/physics/physics_debug_abi.h

- file top: the module draws nothing; the host asks for overlays and uploads the lines through its own line path. The editor's path is rhi::LineVertex pairs, which these lines are byte for byte, so D3D12 and Vulkan agree by construction.
- file top: the packaged game has no line-overlay path and none is added; the C# PhysicsDebugDraw exists for tools and tests.
- file top: a build without JPH_DEBUG_RENDERER (cmake -DAVER_PHYSICS_DEBUG_RENDERER=OFF) answers aver_phys_debug_available() == 0 and every other call does nothing and returns 0.
- file top: units are the engine's (centimetres, +X forward, +Y right, +Z up); colours 0..1; main thread, between steps.
- AVER_PHYS_DBG_BODIES: wireframe shapes of bodies within the radius; meshes and heightfields excluded (see STATIC_TRIANGLES).
- AVER_PHYS_DBG_BOUNDS: world-space bounding boxes (heavy shapes included).
- AVER_PHYS_DBG_COM: centre-of-mass axes.
- AVER_PHYS_DBG_VELOCITY: linear (green) and angular (red) velocity arrows, in metres per second as drawn by Jolt.
- AVER_PHYS_DBG_SLEEP: sleep-test spheres and timers; with the default scheme sleeping bodies turn red.
- AVER_PHYS_DBG_MASS: mass and the box of equivalent inertia.
- AVER_PHYS_DBG_CONSTRAINTS: joint frames, ragdoll joints, vehicle wheels and suspension.
- AVER_PHYS_DBG_LIMITS: joint limit arcs.
- AVER_PHYS_DBG_CONTACTS: contact points and normals of the LAST fixed step (arms per-step capture).
- AVER_PHYS_DBG_SUPPORT_FACES: supporting faces found by the narrow phase (arms per-step capture).
- AVER_PHYS_DBG_SOFT_EDGES: edge, bend, volume, skin and LRA constraints.
- AVER_PHYS_DBG_CHARACTERS: character capsules and ground normal (a CharacterVirtual is not a body).
- AVER_PHYS_DBG_STATIC_TRIANGLES: static mesh / heightfield triangles inside the radius, through Shape::GetTrianglesStart.
- AVER_PHYS_DBG_LINEAR_CAST: where continuous collision ran (arms per-step capture).
- AVER_PHYS_DBG_LABELS: handle, mass and speed text per moving body.
- AVER_PHYS_DBG_ISLANDS: draws bodies coloured by island (implies BODIES and the island scheme).
- colour schemes: MOTION = static grey, kinematic green, dynamic a colour per body (default); SLEEP = active yellow, sleeping red; LAYER = a colour per collision layer; SHAPE = convex green, scaled yellow, compound orange, mesh red; ISLAND = a colour per island; INSTANCE = a colour per body.
- aver_phys_debug_collect: clears the buffers and runs Jolt's draw calls; returns 0 with no world, flags == 0 or no renderer; cameraXyz (engine cm) centres the radius and picks Jolt's level of detail; NULL cameraXyz means no culling; radiusCm <= 0 means unlimited.
- aver_phys_debug_lines: 12 floats per line, (x y z r g b) for start then end, exactly two rhi::LineVertex.
- aver_phys_debug_label_get: position in engine cm, colour 0..1, UTF-8 text truncated to textCap - 1 and always terminated; 0 for a bad index; any out may be NULL.
- aver_phys_debug_arm: contacts, support faces and linear casts are drawn by Jolt inside the step, so they need arming beforehand. While armed, each fixed step clears and refills the capture, so after a frame's steps it holds the LAST step's lines, not all of them. Disarmed, none of it runs. A host calls it every frame the overlay is on. Returns the armed mask.


## modules/physics/include/aver/physics/physics_hair_abi.h

- file top (banner): references docs/plans/wave11/SOFTBODY_HAIR.md section 2 and docs/HAIR.md.
- file top: ADDITIVE, nothing here changes an existing header. Jolt's Hair, HairSettings and HairShaders live inside Aver.Physics, so the module still links Jolt PRIVATE and depends on Core only.
- file top: compute work goes through the plain-C function table the HOST installs (AverPhysComputeHostV1), or with none installed through Jolt's CPU compute backend. The CPU backend runs the same HLSL as C++ on the calling thread: the reference for the tests, a headless server's option, and slow.
- file top: types are int32_t, uint64_t, float, const float* and const int32_t* only.
- file top: the render positions buffer (aver_phys_hair_render_buffer) is hair-local Jolt space, metres, Y up, right-handed; engine position = (-z, x, y) * 100; the ribbon pass does the conversion.
- file top: call from the main thread between aver_phys_step calls, never from a step listener or contact callback. aver_phys_hair_settings_finish may take a second or so for a big groom and may run on a worker thread while the world is not stepping; every other call is cheap. A hair needs a world: aver_phys_init first. aver_phys_shutdown destroys every settings object and hair and forgets the installed host.
- AVER_PHYS_CB_UPLOAD: CPU writes, GPU reads; map(write) discards; ringed by the host.
- AVER_PHYS_CB_READBACK: GPU writes, CPU reads (debug; a GPU host may return NULL from map).
- AVER_PHYS_CB_CONSTANT: small; the host snapshots it at bind time.
- AVER_PHYS_CB_BUFFER: read-only after creation, must carry data.
- AVER_PHYS_CB_RW: read/write on the GPU, optional initial data.
- AverPhysHostHandle: opaque, chosen by the host; 0 is failure.
- AverPhysComputeHostV1.structSize: sizeof, checked. version: 1. user: passed back as the first argument of every callback.
- create_shader: name is one of Jolt's fifteen ("HairIntegrate", ...) or "AverHairWind"; an unknown name returns 0 and the wind kernel is then off.
- create_buffer: count elements of stride bytes; data is null or count * stride bytes (required for CB_BUFFER).
- bind: name is Jolt's binding name ("gPositions"); the host resolves the register for the shader set last. barrier is 1 when the caller wants all earlier writes visible (Jolt's EBarrier::Yes); only meaningful for BIND_RW.
- execute: seals what was recorded.
- wait: a GPU host returns at once; the CPU host has already run it.
- aver_phys_hair_set_compute_host: installs a copy; NULL = Jolt's CPU backend; refused (0) while any settings object exists, so install before the first aver_phys_hair_settings_finish; aver_phys_shutdown resets it; a structSize or version this build does not know is refused.
- aver_phys_hair_host_kind: 0 = CPU backend, 1 = installed host.
- parameter blocks: NaN = leave as is; a block shorter than COUNT keeps the defaults of the rest; longer is refused.
- AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND default [360].
- AVER_PHYS_HAIRP_MAX_DELTA_TIME default [0.0333] s; Jolt clamps the frame time to it, so time slows below 30 fps.
- AVER_PHYS_HAIRP_GRID_X / GRID_Y / GRID_Z default [32] each: velocity grid cells.
- AVER_PHYS_HAIRP_BOUNDS_PADDING_CM default [10]: added on every side of the neutral-pose bounds.
- AVER_PHYS_HAIRP_SIM_FRACTION default [0.1]: default of a material's simulated strand fraction.
- AVER_PHYS_HAIRP_INITIAL_GRAVITY_X / _Y default [0], _Z default [-981]: cm/s^2, engine axes: the gravity the modelled pose already carries.
- material gradients: four floats min, max, minFraction, maxFraction; the value is min at minFraction along the strand, max at maxFraction, clamped outside; the two fractions must differ.
- AVER_PHYS_HAIRM_ENABLE_COLLISION default [1].
- AVER_PHYS_HAIRM_ENABLE_LRA default [1]: long range attachments: keep strands near the modelled pose.
- AVER_PHYS_HAIRM_LINEAR_DAMPING default [2], 1/s. ANGULAR_DAMPING default [2], 1/s.
- AVER_PHYS_HAIRM_MAX_LINEAR_CMS default [1000], cm/s. MAX_ANGULAR_RADS default [50], rad/s.
- AVER_PHYS_HAIRM_GRAVITY (6,7,8,9): gradient default [0.1 1 0.2 0.8].
- AVER_PHYS_HAIRM_FRICTION (10) default [0.2].
- AVER_PHYS_HAIRM_BEND_COMPLIANCE (11) default [1e-7].
- AVER_PHYS_HAIRM_BEND_MULTIPLIER (12,13,14,15): 4 floats at 0%, 33%, 66%, 100% of the strand; default [1 100 100 1].
- AVER_PHYS_HAIRM_STRETCH_COMPLIANCE (16) default [1e-8].
- AVER_PHYS_HAIRM_INERTIA_MULTIPLIER (17) default [10].
- AVER_PHYS_HAIRM_RADIUS_CM (18,19,20,21): gradient default [0.1 0.1 0 1]; collision radius, cm.
- AVER_PHYS_HAIRM_WORLD_INFLUENCE (22..25): gradient default [0 1 0 1]; how much turning the head moves the hair.
- AVER_PHYS_HAIRM_GRID_VELOCITY (26..29): gradient default [0.05 0.01 0 1].
- AVER_PHYS_HAIRM_GRID_DENSITY_FORCE (30) default [0].
- AVER_PHYS_HAIRM_GLOBAL_POSE (31..34): gradient default [0.01 0 0 0.3]; pull towards the modelled pose.
- AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE (35..38): gradient default [1 0 0 0.1].
- AVER_PHYS_HAIRM_SIM_FRACTION (39): default is the settings default; fraction of the material's strands that are simulated.
- AVER_PHYS_HAIRM_GRAVITY_PRELOAD (40) default [0].
- AVER_PHYS_HAIRF_ROOT_TO_SCALP_CM: the largest root-to-scalp distance.
- AVER_PHYS_HAIRF_BOUNDS_*: simulation bounds, cm, hair space, engine axes (1..3 min, 4..6 max).
- AVER_PHYS_HAIRUI_ITERATIONS: solver iterations the update recorded (0 when dt rounded to none). DISPATCHES: compute dispatches recorded, wind included. UPDATES: updates since create. WIND: 1 when the update recorded the wind kernel.
- AVER_PHYS_HAIRUF_DT_USED: seconds after Jolt's clamp.
- aver_phys_hair_settings_begin: a builder id. Stage strands, scalp, materials and params in any order, then finish. A cancelled or finished builder id is dead.
- aver_phys_hair_settings_strands: pointsXyz is cm, scalp-mesh space, engine axes, 3 floats per vertex. strandStart has strandCount + 1 entries: first 0, last vertexCount, strictly increasing by at least 3 (a strand has 3..255 vertices and no two neighbours coincide). strandMaterial 0..255 per strand (null = all 0). invMass per vertex or null (1 each; 0 pins); the first vertex of every strand is always pinned to the scalp. Repeated calls replace the earlier strands.
- aver_phys_hair_settings_scalp: triangle mesh the roots attach to, skinned by up to 4 joints per vertex. joints4 / weights4 are 4 per vertex; a joint is an index into the SCALP JOINT LIST that aver_phys_hair_set_palette fills, in the same order. Weights are renormalised; both null = a single joint 0. jointCount 0 is treated as 1. Every root must be within reach of a triangle. Repeated calls replace the scalp.
- aver_phys_hair_settings_material / _params: n <= the COUNT; a material index used by a strand and never described keeps the defaults.
- aver_phys_hair_settings_finish: splits the strands into render and simulation strands, optionally resamples every strand to resampleVertices (0 = none, else 3..255), initialises and creates the compute buffers on the installed host. Returns the SETTINGS handle (> 0), 0 on refusal. outRootToScalpCm (may be null) receives the largest root-to-scalp distance. The builder id is consumed whatever the outcome. The split is quadratic in render strands times simulated strands (UNVERIFIED: 0.2 to 1 s for 20k strands): call on a worker.
- aver_phys_hair_settings_destroy: refused while an instance uses the settings.
- aver_phys_hair_settings_set_material: live, re-read by every update; the modelled pose, the strand split and anything baked at finish are not.
- aver_phys_hair_settings_info: AVER_PHYS_HAIRI_* ints and AVER_PHYS_HAIRF_* floats; either array may be null.
- aver_phys_hair_settings_render_strands: the render strand table in the order the render positions buffer holds the strands: {first render vertex, vertex count} per strand; writes up to maxStrands pairs (null = count only); returns the number of render strands.
- aver_phys_hair_create: world16 is the hair space to world matrix, row-major, the engine's row-vector convention (the head entity's world); a scale in it is stripped. layer is the user collision layer 0..15 the hair collides as. Returns the hair handle, 0 on refusal. The hair is teleported (rest pose, zero velocity) by its first update.
- aver_phys_hair_teleport: default pose and zero velocity at the next update, wherever the hair now is.
- aver_phys_hair_set_palette: the engine's skinning matrices (model x inverse bind, row-major, 16 floats each) of the scalp joints, in scalp joint list order; jointCount must equal the settings' count; identity until the first call.
- aver_phys_hair_set_wind: world wind cm/s (engine axes), drag rate 1/s, turbulence amplitude cm/s. Zero wind, or zero drag, records no kernel.
- aver_phys_hair_update: records one simulation step (wind kernel, then Jolt's Hair::Update) into the shared queue; dt is the frame time in seconds; Jolt clamps it to MAX_DELTA_TIME.
- aver_phys_hair_flush: executes and waits for everything the updates of this tick recorded; once per tick, after all updates.
- aver_phys_hair_render_buffer: float3 per render vertex, hair-local Jolt space (metres, Y up, right-handed); 0 for an unknown hair; with the CPU backend (no host installed) there is no host handle and the answer is 0.
- aver_phys_hair_info: AVER_PHYS_HAIRUI_* ints and AVER_PHYS_HAIRUF_* floats about the last update; either array may be null.
- aver_phys_hair_read_positions: debug and tests; simulated vertices in cm, hair space, engine axes, in the settings' simulated vertex order; render positions in render buffer order. Either output may be null. Slow (waits for the queue). Refused when the installed host cannot map a readback buffer.


## modules/physics/include/aver/physics/physics_shape_builder_abi.h

- file top: a "shape handle" used to mean a static triangle mesh (physics_abi.h: aver_phys_create_mesh_shape); it now names any shape made here, and aver_phys_release_mesh_shape and aver_phys_add_mesh_shape_body accept any of them.
- file top: a shape handle is NOT a body handle; the two tables are separate by design, as the mesh table always was. Hand a shape to a shape function and a body to a body function.
- file top: builders and release are safe from any thread (level streaming builds on workers); body functions are main thread only.
- file top: a shape stays alive while a body uses it, so releasing right after aver_phys_body_from_shape is correct.
- file top: units are the engine's: centimetres, +X forward, +Y right, +Z up, left-handed; quaternions (x, y, z, w). A NULL quaternion is identity, a NULL position is zero. Half extents and radii are sizes.
- file top: a capsule's height is TOTAL (both caps); a cylinder's and tapered cylinder's is FULL end to end. All stand along +Z with topRadius at +Z.
- file top: failures return 0 (or -1 where 0 is a real answer) and set aver_phys_last_error.
- file top: parameter blocks: any prefix; a longer one is refused; unset entries keep the default; a negative value means the shape's own default (EDGE_ANGLE_DEG alone reads negative as "every edge active").
- AVER_PHYS_SP_CONVEX_RADIUS: cm, default 5 (triangle 0); clamped by Jolt to the smallest extent.
- AVER_PHYS_SP_DENSITY: kg/m^3, default 1000.
- AVER_PHYS_SP_HULL_TOLERANCE: cm, default 0.1.
- AVER_PHYS_SP_EDGE_ANGLE_DEG: mesh active-edge angle, default 5; negative = every edge active.
- AVER_PHYS_SP_BUILD_QUALITY: mesh; 0 favours runtime performance, 1 favours build speed.
- AVER_PHYS_SP_MAX_TRIS_PER_LEAF: mesh, 1..8, default 8.
- leaves: `surface` is a physical-surface id (0 = none), see physics_surface_abi.h.
- aver_phys_shape_triangle: a thin convex triangle (not a mesh): 9 floats, convex radius 0 unless SP says otherwise.
- aver_phys_shape_plane: halfExtentCm (1 to 1e6) bounds its broadphase box. Static or kinematic bodies only.
- aver_phys_shape_empty: no volume, collides with nothing: a placeholder, or a kinematic anchor for a joint.
- aver_phys_shape_convex_hull: at least 4 non-coplanar points; Jolt reduces more than 256 vertices itself.
- aver_phys_shape_mesh: palette is up to 32 surface ids; triPaletteIndex is one byte per input triangle (NULL = all 0); triangles skipped for bad indices are dropped from all arrays together; triUserData is one int32 per triangle (NULL = none) and turns on Jolt's per-triangle user data (about 25% more memory).
- decorators and compounds: children are shape handles. COMPOUND_STATIC = 0, COMPOUND_MUTABLE = 1.
- aver_phys_shape_offset_com: the centre of mass moves by offsetCm; the geometry does not.
- aver_phys_shape_scaled: Jolt's ScaleShape: a scale the shape cannot wrap is baked when Jolt can, refused when it cannot. Zero components are refused.
- aver_phys_shape_compound: 1 to 65536 children; per-child position and rotation in the compound's frame (NULL arrays = zero / identity); per-child user data (NULL = 0). A MUTABLE compound is single-owner: the first aver_phys_body_from_shape binds it to that body and a second use is refused.
- aver_phys_shape_of_body: a new handle on the body's current shape (the caller releases it); refused for a character or a soft body.
- SHAPE_TYPE_* values: UNKNOWN 0, BOX 1, SPHERE 2, CAPSULE 3, CYLINDER 4, TAPERED_CAPSULE 5, TAPERED_CYLINDER 6, CONVEX_HULL 7, TRIANGLE 8, PLANE 9, EMPTY 10, MESH 11, HEIGHTFIELD 12, STATIC_COMPOUND 13, MUTABLE_COMPOUND 14, ROTATED_TRANSLATED 15, SCALED 16, OFFSET_COM 17, SOFT_BODY 18.
- aver_phys_shape_info float block (SI): shape-local cm, kg, kg cm^2. Mass is volume times density (0 for a mesh). Principal inertia is in the principal frame given as a quaternion; bounds are per-axis sorted.
- SII_TYPE: AVER_PHYS_SHAPE_TYPE_*. SII_CHILDREN: direct children of a compound, else 0. SII_LEAVES: recursive. SII_BYTES: saturated to int32.
- aver_phys_shape_info: writes up to min(nF, SI_COUNT) floats and min(nI, SII_COUNT) ints; either pointer may be NULL with its count 0.
- body parameter block (BP) defaults: LAYER collision layer, default 0; SENSOR default 0; INERTIA_FROM_BOUNDS default 0, forced to 1 for a mass-less mesh given a mass; ENHANCED_EDGES default 0 (Jolt's enhanced internal edge removal); ACTIVATE default 1.
- aver_phys_body_from_shape: origin originCm, quatXyzw NULL = identity. motionType is AVER_PHYS_MOTION_*. massKg <= 0 takes the shape's volume times density. Plane, heightfield and a mesh without a mass are refused as dynamic. A MESH with massKg > 0 is accepted as dynamic or kinematic: its centre of mass moves to the bounds centre and its inertia is a box's. A dynamic mesh collides with convex shapes only and falls through meshes and heightfields (Jolt has no mesh-mesh collision). aver_phys_body_position reports the centre of mass; aver_phys_body_origin reports the origin.
- aver_phys_body_set_shape: swaps the shape; the body ORIGIN stays where it was (a new centre of mass moves the centre, not the pose a model hangs from). keepMass keeps the old mass, otherwise the new shape's; a shape with no mass keeps it regardless. Refused for a character, a soft body, a vehicle chassis and a ragdoll part.
- aver_phys_body_set_scale: ABSOLUTE scale relative to the unscaled shape. Mass follows volume unless keepMass.
- compound edits (aver_phys_body_compound_*): each re-centres the compound and keeps the body's mass. Adding or removing invalidates every raw sub-shape id taken before and shifts child indices; the last child cannot be removed. compound_set_child keeps a NULL position or rotation as it is. compound_add returns the new child index or -1; compound_count is -1 when the body is not a compound. Applies to a body whose shape IS a mutable compound it owns.
- aver_phys_body_triangles: the body's collision triangles intersecting the world box (NULL region = the body's AABB): 9 floats each in engine cm, one surface id per triangle (outSurface may be NULL), truncated at maxTriangles. Returns the count.
- aver_phys_vehicle_set_chassis_shape: a vehicle chassis from any shape with a mass, in the vehicle origin frame; rebuilt under the same handle like aver_phys_vehicle_set_chassis_boxes (mass and centre of mass kept). Refused after _finish.


## modules/physics/include/aver/physics/physics_softbody_ext_abi.h

- file top: ADDITIVE. physics_softbody_abi.h (AVER_PHYS_SB_ABI_VERSION 2) is not edited, and a body that never calls anything here behaves exactly as before.
- file top: the builder calls extend a builder from aver_phys_sb_builder_begin and are staged until aver_phys_sb_builder_finish; the live calls take the handle finish returned. The Ext state of a builder is erased at finish or cancel.
- file top: types are int32_t, float, const float* and const int32_t* only. Every function returns 1 on success and 0 on refusal (aver_phys_last_error says why) unless it says it returns a count.
- file top: the scope list was: inflatables, tetrahedral volumes, long range attachments, Cosserat rods, Jolt's own constraint generator, grab and release, contact policy, sensors, a collision group, and buoyancy.
- file top: "units and axes are the engine's everywhere": centimetres, cm/s, kilopascal, +X forward, +Y right, +Z up.
- extra parameter defaults: pressure [0] kPa at rest volume; skin enabled [1]; skin distance scale [1] (0 = hard skin); collision group [0]; soft / other inverse mass scale and other inverse inertia scale [1] (0 = infinite mass); fluid density [0] kg/m^3 (0 = no buoyancy); fluid drag [1]; buoyancy flags [1]. NaN entry = leave as is (a rebuild with replaceBody keeps the replaced body's value).
- aver_phys_sbx_builder_volumes: the rest volume is measured from the staged rest positions at finish; the constraint is abs(signed volume), so winding does not matter. compliance may be null (0 = incompressible). Repeated calls append. A body that stages volumes, rods or auto constraints may have no faces: it gets one extra face over its first three vertices (so it needs at least 3 vertices).
- aver_phys_sbx_builder_lra: explicit long range attachments, pairs2 = {kinematic vertex, dynamic vertex} per pair, maxDistCm per pair. Repeated calls append.
- aver_phys_sbx_builder_lra_auto: LRA from the closest kinematic vertex (inverse mass 0) over the staged edges and rods, for every dynamic vertex. mode 0 off, 1 Euclidean, 2 geodesic. distanceMultiplier >= 1 (1.01 = 1% slack).
- aver_phys_sbx_builder_rods: Cosserat rods: ab = 2 vertices per rod; rodPairs2 = pairs of rod indices (0-based within THIS call) joined by a bend-twist constraint. Compliance arrays may be null (0). A rod not named in any pair is refused (it would spin forever). Repeated calls append rods; the indices of one call stay local to it.
- aver_phys_sbx_builder_rod_chain: vertices [firstVertex, firstVertex + vertexCount) joined by vertexCount - 1 rods and vertexCount - 2 bend-twist pairs between neighbours (vertexCount >= 3).
- aver_phys_sbx_builder_auto: Jolt's own CreateConstraints over the staged faces. attrs5 = per vertex {compliance, shearCompliance, bendCompliance (negative = no bends), lraMode (0 off, 1 Euclidean, 2 geodesic), lraMultiplier}; attrCount may be less than the vertex count (the last repeats, as in Jolt). bendType 0 none, 1 distance (an extra edge), 2 dihedral. It makes the edges and bends itself, so finish refuses a builder that also staged edges or bends.
- aver_phys_sbx_builder_params: NaN = leave as is. A pressure on a mesh that is not closed (or is inside out) is ignored with a log line at finish.
- aver_phys_sbx_slots: after finish, the final Jolt index of each staged item (Optimize reorders them). kind 0 volume, 1 lra (explicit pairs first, then lra_auto's), 2 rod, 3 rod pair (bend-twist). Writes up to maxCount ints (null = count only) and returns the number of items. Valid for the body the builder made.
- aver_phys_sbx_set_pressure: pressure in kPa at REST volume (Jolt's n R T is derived from the rest volume measured at finish). 0 = off. Refused (Unsupported) for a mesh that is not closed or is inside out, negative or non-finite.
- aver_phys_sbx_volume: rest and current volume in cm^3 of the body's faces; now is negative when the shape is inside out.
- aver_phys_sbx_set_skin: skin constraints on or off, and the max distance multiplier (0 = hard skin).
- aver_phys_sbx_set_inv_mass: per-vertex inverse mass (0 = pinned). Recomputes the body's mass and the inverse mass of every rod that touches a vertex. The caller's own copy should be updated too if it rebuilds the body.
- aver_phys_sbx_attach: the vertex becomes kinematic and is carried to the target by its velocity at the start of every fixed step (frame-rate independent). One way: the vertex follows, the target feels nothing. maxSpeedCmS <= 0 = no cap. Attaching a vertex that is already attached replaces its target and keeps the mass it had before.
- aver_phys_sbx_attach_to_body: follows a point of another body; localOffsetXyzCm is in that body's own frame (its position and rotation). A stale otherBody detaches the vertex (logged once).
- aver_phys_sbx_move_attachment: a plain attach's target.
- aver_phys_sbx_detach: restoreInvMass < 0 = what the vertex had before it was attached. Zeroes the vertex velocity and wakes the body.
- aver_phys_sbx_set_contact_policy: scales: 0 = infinite mass, 1 = unchanged, 2 = half the mass. Keep a pair's scales constant over time. Applied when the soft body meets a rigid body.
- AVER_PHYS_SBX_PAIR_NORMAL 0; PAIR_SENSOR 1 = no response, enter and exit events; PAIR_IGNORE 2 = the contact is rejected.
- aver_phys_sbx_sensor_count / _get: sensor contacts, polled and cleared at the top of aver_phys_step like contacts: Jolt sensor bodies (the aver_phys_add_sensor_* ones) and pairs set to SENSOR. outEntered is 1 for an enter, 0 for an exit. A body that is asleep or deactivated reports nothing and keeps its inside set.
- aver_phys_sbx_set_collision_group: 0 = none; otherwise the soft body ignores every rigid body whose group id equals this (a ragdoll's handle is its group id, so a cape ignores its wearer's ragdoll parts without a layer).
- aver_phys_sbx_set_buoyancy: fluidDensityKgM3 0 = off. Hydrostatic pressure on every submerged face of a closed mesh plus quadratic drag on any mesh (drag 1 is a flat plate), evaluated in the fixed step. A buoyant body is kept awake while any face is submerged. flags: WORLD_WATER = the host water sampler, else the global water plane (aver_phys_set_water_plane); OWN_PLANE = the plane of aver_phys_sbx_set_water_plane, which wins.
- aver_phys_sbx_submerged: the last step's area-weighted submerged fraction (0..1) and displaced volume (closed meshes; 0 otherwise).
- aver_phys_sbx_rod_state: rod i in the FINAL order (see aver_phys_sbx_slots kind 2). outQuatXyzw: 4 floats per rod, the world orientation in engine axes such that the engine +X axis rotated by it is the rod's direction (first vertex toward second); its twist follows the Bishop frame. outAngVelRadS: 3 floats per rod, rad/s. Either may be null. Returns the number of rods written.


## modules/physics/include/aver/physics/physics_state_abi.h

- file top: a world that never calls anything here steps exactly as before; the only cost is one boolean test per fixed step for the hash log.
- file top, WHAT A SNAPSHOT IS: a byte blob holding everything the simulation DERIVES between inputs: Jolt's bodies, contact cache and constraints (joints, ragdolls, vehicle wheels, engine and gearbox), every character's CharacterVirtual state, and engine-side state Jolt cannot see (the fixed-step accumulator, vehicle driver-aid state, a character's ground carry).
- file top: INPUTS ARE NOT STATE and are never in a snapshot: pedals set this frame, forces, impulses, velocities a game writes, kinematic targets, tune values, ragdoll mode and strength, wind. A rollback layer re-issues them for each resimulated tick.
- file top: statics are left out unless asked for (AVER_PHYS_SNAP_STATICS).
- file top, RESTORE IS ALL-OR-NOTHING: it validates first. Every captured body must still exist with the same Jolt id; every captured character, vehicle, joint and ragdoll must still exist; no constraint may have been added or removed since (Jolt restores constraints by array index and would silently restore the wrong one). Only then does it call Jolt. A refusal changes nothing and says why through aver_phys_snapshot_last_result(). Bodies ADDED since the capture are allowed by default and keep their current state (AVER_PHYS_RESTORE_STRICT refuses them).
- file top, A SNAPSHOT IS NOT A SAVE FILE: handles change when a level reloads, so a snapshot only restores into a world built the same way. aver_phys_snapshot_read / _import exist for transport (a netcode peer, a test). A snapshot from a single-precision build is refused by a double-precision one (BUILD_MISMATCH); aver_phys_build_string is what two peers compare.
- file top: units are the module's; returns are 1/0 with aver_phys_last_error() as the reason channel, except the restore family, whose richer result has its own getter and never uses negative returns (the inversion trap ErrorCodes.hpp documents).
- AVER_PHYS_SNAP_BODIES: dynamic and kinematic bodies, a soft body's vertices included.
- AVER_PHYS_SNAP_CONSTRAINTS: joints (a broken one stays broken or un-breaks), ragdolls, vehicle wheels / engine / gearbox.
- AVER_PHYS_SNAP_ENGINE: the accumulator, vehicle driver-aid state and a character's ground carry.
- AVER_PHYS_SNAP_STATICS: also static bodies; off by default (~48 bytes each, 65k statics = 3 MB per snapshot).
- AVER_PHYS_SNAP_ALL = 0x1F.
- AVER_PHYS_RESTORE_STRICT: also refuse when bodies, characters, vehicles or joints were ADDED since the capture.
- RESTORE results: OK 0; BAD_HANDLE -1 = no such snapshot, or it belongs to a world that is gone; BAD_HEADER -2 = magic, ABI layout or size wrong: not a snapshot, or truncated; BUILD_MISMATCH -3 = precision, Jolt version, state layout version or determinism build differ; TOPOLOGY -4 = a captured body, character, vehicle, joint or ragdoll is gone, or the constraint set changed; CORRUPT -5 = the payload does not match its checksum, or Jolt refused the stream.
- SI slots: FLAGS = the AVER_PHYS_SNAP_* parts held; BODIES = bodies Jolt saved (character inner bodies included); CHARACTERS, VEHICLES, JOINTS, RAGDOLLS = counts; TOPOLOGY_EPOCH, CONSTRAINT_EPOCH; PRECISION = AVER_PHYS_PRECISION_* of the capturing build; STEPS_LOW / STEPS_HIGH = fixed steps since the world was created at the capture (low 32 bits, then high 32); BYTES = the blob size (header included).
- aver_phys_snapshot_capture: `reuse` of a live snapshot overwrites it in place (the byte buffer keeps its capacity, so a ring of N snapshots allocates N buffers once) and returns the same handle; 0 or a dead handle allocates a new one. flags 0 means AVER_PHYS_SNAP_ALL. Returns 0 with no world, or for a flag outside the known set (aver_phys_last_error says which).
- aver_phys_snapshot_restore: on success the event queues (contacts, overlaps, vehicle events, joint breaks, soft-body impacts) are cleared, because the resimulated steps will produce them again; aver_phys_step_total() returns the capture's step count.
- aver_phys_snapshot_release: 1; 0 for a dead handle.
- aver_phys_snapshot_release_all: releases every snapshot of the current world and returns how many there were. Hosts call it when a level is unloaded: a snapshot names the level's bodies, which are about to go, so none could be restored afterwards anyway.
- aver_phys_snapshot_count: live snapshots in the current world.
- aver_phys_snapshot_bytes: the blob size; 0 for a dead handle.
- aver_phys_snapshot_read: copies the blob (header and payload) for disk or network. Call with outData == NULL for the size; otherwise at most cap bytes are written and the count returned (0 when cap is smaller than the blob: it is never cut short).
- aver_phys_snapshot_import: validates a blob (header, build, checksum) and returns a handle usable with restore; 0 on failure, with last_result saying BAD_HEADER, BUILD_MISMATCH or CORRUPT. Needs a world: a snapshot belongs to the world alive when it is imported.
- aver_phys_snapshot_info: min(n, COUNT) written; returns the count written, 0 for a dead handle.
- aver_phys_snapshot_hash: FNV-1a 64 of the payload (the blob without its header, so the capture time and the step count are not in it); 0 for a dead handle.
- aver_phys_snapshot_diff: the offset of the first payload byte at which two snapshots differ; -1 when equal; -2 for a dead handle. When the sizes differ and one is a prefix of the other, the shorter size. Used to find WHERE two runs diverge.
- aver_phys_state_hash: the hash a capture made now with `flags` would have (0 = ALL), without keeping the capture. 0 with no world.
- aver_phys_hash_log: non-NULL creates (truncating) the file and, from the next fixed step on, writes one "step hash" line (the decimal fixed-step number, then 16 hex digits of the ALL-parts hash taken at the end of the step), so two runs diff directly. NULL: stop. 1 on success; 0 when the file cannot be opened or there is no world. The log closes when the world is shut down. The hash costs about a snapshot per fixed step: a diagnostic, off by default.
- aver_phys_step_exact: runs exactly `steps` (1..600) fixed steps, ignoring the accumulator and leaving it as it was. Returns the steps run; 0 for a count outside the range or no world. The rollback layer re-simulates ticks with it.
- aver_phys_step_total: fixed steps since this world was created (restore sets it back); 0 with no world.
- aver_phys_accumulator_set: for a replay that must restore a mid-frame position; clamped into [0, 8 fixed steps]; 1/0.
- SHAPE BINARY CACHE, purpose: a mesh or heightfield shape is mostly its BVH, which costs far more to build than to read back. modules/world/include/aver/world/PhysicsShapeCache.hpp wraps the blob in an AVR1 "PSHP" file under <project>/Saved/DerivedDataCache/Physics.
- SHAPE BINARY CACHE, a restore is always a MISS, never a failure: a wrong key, cache version, Jolt version, precision, a truncated or damaged blob all answer 0 and the caller builds the shape the slow way.
- SHAPE BINARY CACHE, WHAT CAN BE SAVED: a mesh built by aver_phys_create_mesh_shape (no per-triangle user data) and a heightfield built by aver_phys_create_heightfield_shape / aver_phys_heightfield_shape / aver_phys_add_heightfield's builder, in both cases ONLY when the shape carries no physical-surface materials (a palette names live surface ids, which a file cannot hold). Anything else answers 0 from save. A restored heightfield is a plain collision shape and is not editable (aver_phys_heightfield_set_heights refuses it), because the edit metadata lives with the process that built the field.
- SHAPE BINARY CACHE, THE KEY is the caller's: a hash of everything the shape was built from (the host uses mesh path, size, mtime and the collision cache version). It is stored in the blob and compared on restore, so a blob for another mesh or an older source is a miss even if the file name collides.
- AVER_PHYS_SHAPECACHE_VERSION 1u: what aver_phys_shape_cache_version() answers today.
- aver_phys_shape_cache_version: bumped by any change to unit conversion, winding or the blob layout.
- aver_phys_shape_cache_enable: 1 when the host should use the cache. Default 0: off until measured on real levels; PHYSICS.SHAPECACHE in the project, or the AVER_PHYS_SHAPE_CACHE=1 environment variable for tools. Process-wide; the cache calls work whatever it says.
- aver_phys_create_heightfield_shape: aver_phys_add_heightfield's construction (a square sampleCount * sampleCount grid, row-major, the v1 physics order, cm) minus the body; a shape handle in the same table as aver_phys_create_mesh_shape, placed by aver_phys_add_mesh_shape_body. 0 on failure.
- aver_phys_shape_save: returns the bytes written, or with outData == NULL the bytes needed; 0 when the handle is not a saveable mesh or heightfield, or cap is smaller than the blob (never cut short).
- aver_phys_shape_restore: the new shape handle, or 0 for a miss (wrong key, version, build or precision, or a truncated or damaged blob). The shape is the same bits the original was: queries and bodies behave identically. Needs a world.


## modules/physics/include/aver/physics/physics_surface_abi.h

- file top, SURFACE IDENTITY: a surface is a name hashed to a 32-bit id, the id the engine already uses: Aver.Framework.Surface.IdOf (FNV-1a over the UTF-8 name, never 0) and the graph rows an_surfaceid / an_landscapesurface hand scripts that same number. The registry keys on exactly that hash, so a raycast on a mesh, a contact on a crate and Landscape.Sample on the terrain all answer in one id space. Names are case-sensitive; 0 is "no surface".
- file top, MATERIALS: shapes carry a surface where Jolt has a slot: one per convex leaf (the `surface` argument of the builders in physics_shape_builder_abi.h), a palette of up to 32 per mesh, up to 256 per heightfield. A surface's friction and restitution are shared by every shape that uses it. Building a shape with a surface id interns it, so ids never need registering first; registering only adds a name and numbers.
- file top, WHAT A CONTACT USES: friction and restitution are combined from the leaf material of each side when it has a number (>= 0), else the body's own, by Jolt's rules (friction sqrt(f1 f2), restitution max(r1 r2)). Until any surface has ever been interned the same arithmetic runs on the body values alone, so a project that never uses a surface behaves bit for bit as before. A surface sampler (physics_water_abi.h) still runs after the combine and wins on its terrain body.
- file top, BODY LABEL: aver_phys_body_set_surface is identity for queries and does NOT change contact friction; pass copyPhysics = 1 to copy the registered numbers into the body once.
- file top, THE LABEL lives in bits 32 to 63 of the body's user data; the entity id (aver_phys_set_entity) keeps the low 32, and every reader truncates to int32, so the two coexist. Static-to-movable rebuilds and vehicle chassis swaps keep it.
- file top, WHICH SURFACE A HIT NAMES: the leaf material at that sub-shape if it is a surface, else the body label, else 0. A character's ground is the body and sub-shape Jolt remembers under its feet; soft bodies have no leaf materials.
- file top, SUB-SHAPE IDS: HITI_SUB_SHAPE values are raw Jolt values, valid only while the shape is unchanged. A mutable-compound edit, aver_phys_body_set_shape / set_scale, a heightfield edit or a released shape invalidates every id taken before; callers re-query. HITI_CHILD and HITI_CELL_* are the stable-per-shape identities.
- file top, THREADING: registry writes and tune / set_surface are between-step calls on the thread that steps. A tune during aver_phys_step from another thread is a data race.
- file top: failures return 0 (or -1 where 0 is a real answer) and set aver_phys_last_error.
- AVER_PHYS_HITF_FRICTION / RESTITUTION: the numbers the resolved surface carries (-1 when it defers to the body or there is no surface); these are not necessarily what a contact applied (a body label does not change contact friction).
- AVER_PHYS_HITI_CHILD: index of the top-level compound child the sub-shape lies in (-1 when the shape is not a compound). CHILD_USER_DATA: that child's user data.
- AVER_PHYS_HITI_TRIANGLE: a mesh's per-triangle user data when the mesh was built with it (aver_phys_shape_mesh triUserData), else -1.
- AVER_PHYS_HITI_CELL_X / CELL_Y: the heightfield cell in physics order, else -1.
- AVER_PHYS_CONF_CLOSING_SPEED_CMS: the relative speed of the two bodies at the contact point along the contact normal, sampled when the contact began (before the solver); positive = approaching.
- AVER_PHYS_CONF_FRICTION / RESTITUTION: the values the contact applies, after the leaf materials and any surface sampler.
- aver_phys_surface_id: FNV-1a 32 over the UTF-8 bytes; 0 for NULL or empty; a zero hash becomes 1. Golden values: "Ice" -714267872, "Grass" 1479220765, "Rubber" 1606840731, "Wood" 81868168. Pure.
- aver_phys_surface_register: upserts a named surface and returns its id, or 0 (refused, logged) for a NULL or empty name, a non-finite number, or a hash collision with a different registered name. friction and restitution below 0 mean "defer to the body"; both clamp to 10. tag is an opaque number for games (a footstep group, an FX class).
- aver_phys_surface_tune: live: shapes share the material, so every body on it takes the new value at its next contact evaluation. 0 (BadHandle) for an id nothing has used. Works on surfaces registered or interned by a shape.
- aver_phys_surface_info: an id interned only by a shape has an empty name and the numbers -1, -1, 0. Any out-pointer may be NULL. The name is UTF-8, NUL-terminated, truncated to nameCap bytes (C# byte[]).
- aver_phys_surface_count: registered surfaces (those with a name). aver_phys_surface_at: the id at a dense index in registration order (0 out of range).
- aver_phys_surface_reset: forgets every name and returns every surface's numbers to unset (-1, -1, 0). The materials stay alive for the shapes holding them, which then defer to their bodies. The registry otherwise outlives aver_phys_shutdown.
- aver_phys_body_set_surface: labels a body (or character) with `surface`; 0 clears it. copyPhysics = 1 also copies the surface's registered friction and restitution (when >= 0) into the body through the ordinary setters. Returns 0 for a dead handle.
- aver_phys_body_surface: the label; 0 = none, -1 for a dead handle (a real id of exactly -1 reads the same: one hash in four billion).
- aver_phys_raycast_detail: the same cast as aver_phys_raycast_ex (layerMask, ignoreBody, AVER_PHYS_QUERY_* flags in physics_queries_abi.h: sensors are skipped unless asked). Returns the hit body's handle or 0 for a miss; the blocks are written only on a hit. nF / nI are the capacities of outF / outI (entries beyond a block's COUNT are not touched).
- aver_phys_subshape_info: the same blocks for any (body, sub-shape) pair from a hit, a contact or the ground; POINT, NORMAL and DISTANCE_CM are zero. 1 on success.
- aver_phys_contact_detail: contact i of the last aver_phys_step, index-aligned with aver_phys_contact_get and cleared with it.
- aver_phys_character_ground_detail: what a character stands on (or touches, when not supported): POINT is the ground position, NORMAL the ground normal, from Jolt's ground fields. 1 when there is a ground body; 0 in the air or on another character (not an error).
- aver_phys_vehicle_wheel_surface: what a finished vehicle's wheel is touching. 1 with contact, 0 in the air.
- AVER_PHYS_HITF / HITI / CONF / CONI: "Never invent an id: pass back what a hit, a contact or the ground gave you." (sub-shape ids).


## modules/physics/include/aver/physics/physics_terrain_abi.h

- file top: aver_phys_add_heightfield (physics_abi.h) stays exactly as it was and is now built through the same code with an empty parameter block, so a terrain made either way is bit for bit the same.
- file top: aver_phys_heightfield_shape returns a handle in the generic shape table (physics_shape_builder_abi.h); aver_phys_body_from_shape (static, mass 0) makes the body. A heightfield shape is static only; a dynamic request is refused there.
- file top: units and orientation are aver_phys_add_heightfield's: centimetres. A sample (column x, row y) lands at engine (cx - y * spacing, cy + x * spacing, cz + height) when the body is created at origin (cx, cy, cz). A cell is the square between four neighbouring samples: (cols - 1) * (rows - 1) of them, cell (x, y) between columns x..x+1 and rows y..y+1.
- file top, HOLES: a sample that is NaN, infinite or FLT_MAX is a hole. Jolt's rule: a cell is dropped whole when a corner on its diagonal is a hole (the diagonal runs from a cell's (x, y) corner to its (x+1, y+1) corner), and a hole on either other corner drops one of the cell's two triangles. So a hole at sample (x, y) removes cells (x, y) and (x-1, y-1) entirely and half of cells (x-1, y) and (x, y-1). Nothing collides there and a ray passes through.
- file top, NON-SQUARE: Jolt's field is square. The shape is built N x N with N = max(cols, rows), the extra samples are holes, so memory is N^2 samples whatever the rectangle: an 8:1 strip costs eight times a square of its short side.
- file top, CELL SURFACES: cellPaletteIndex is one byte per cell indexing palette (up to 256 surface ids, 0 = none); NULL gives every cell palette[0]. A hit on the field reports the cell's surface and its (x, y) through aver_phys_raycast_detail (physics_surface_abi.h).
- file top, EDITS: go through Jolt's documented race-free route: the shape is cloned, the clone is edited and swapped onto the body with SetShape, so a query in flight finishes on the old one and the body handle keeps working. A clone costs a copy of the field per call: batch an edit per frame. A heightfield edit invalidates every raw sub-shape id taken before it (the cell coordinates stay valid). Edits are between-step calls on the thread that steps.
- file top: failures return 0 and set aver_phys_last_error.
- AVER_PHYS_HP_BLOCK_SIZE: 2 to 8, default 2. Bigger saves memory and slows queries.
- AVER_PHYS_HP_BITS_PER_SAMPLE: 1 to 16, default 8.
- AVER_PHYS_HP_EDGE_ANGLE_DEG: active-edge angle, 0 to 90, default 5.
- AVER_PHYS_HP_EDIT_HEADROOM_CM: default 0; widens the editable height range by this much each way.
- HP: any prefix; a longer block is refused; a negative entry is the default.
- AVER_PHYS_HF_WAKE: wake the dynamic bodies over the edited area so they fall into a new crater.
- aver_phys_heightfield_shape: builds from cols * rows samples (2 to 16384 each) spaced spacingCm apart. QUANTISATION: Jolt compresses each block relative to its own height range, then edits clamp to the global range fixed here. With headroom 0 that range is the lowest to the highest sample, so a crater below the lowest sample cannot be made later; EDIT_HEADROOM_CM widens the range. palette is up to 256 surface ids. Returns a shape handle.
- aver_phys_heightfield_set_heights: sets a rectangle of w * h samples at column x0, row y0 on the heightfield body (row-major, cm, NaN = hole). The body's shape must BE the heightfield (a scaled one is refused). Values outside the encoded range are clamped, not refused: the call returns 1 and HFI_CLAMPED_TOTAL counts them. Jolt re-compresses a border one block wide on the low side of the rectangle, so a few neighbouring samples lose a little precision. flags is AVER_PHYS_HF_*.
- aver_phys_heightfield_get_heights: reads a rectangle of w * h samples back (row-major, cm). Values are decompressed, so they differ slightly from what the field was made with; holes read FLT_MAX.
- aver_phys_heightfield_set_cell_surfaces: rewrites the surface of a rectangle of w * h CELLS at cell (x0, y0): one palette index per cell (NULL = palette[0]). The palette is merged into the field's own; more than 256 surfaces in all is refused and nothing changes. A field built with no palette reports surface 0 for the cells this call does not touch.
- AVER_PHYS_HFI_SPACING_CM: cm. MIN_HEIGHT_CM: the lowest height an edit can encode. MAX_HEIGHT_CM: the highest. CLAMPED_TOTAL: samples clamped by edits since the shape was made (a count, as a float).
- AVER_PHYS_HII_PADDED: Jolt's square side, a multiple of the block size. HII_MATERIALS: surfaces in the field's own list.
- aver_phys_heightfield_info: writes up to min(nF, HFI_COUNT) floats and min(nI, HII_COUNT) ints; either pointer may be NULL with its count 0. The body's shape must be a heightfield made by this module.


## modules/physics/include/aver/physics/physics_world_abi.h

- file header: a caller that never touches this header gets the world it always got; aver_phys_init() with no arguments reads the pending config, whose defaults are the values a world is built with.
- file header: units and axes are the engine's (cm, cm/s, seconds, degrees); Jolt's metres never cross this boundary; the settings table converts.
- file header: setters return 1/0 and aver_phys_last_error() (physics_abi.h) is the reason channel. Main thread, between steps, like the rest of the ABI.
- file header: one table drives everything: the live settings, the PHYSICS.* project keys, the phys.* console variables, the C# PhysicsSetting enum and the editor's settings page are all produced from the table in PhysicsConfig.cpp. The name list in OcProject.cpp (which may not depend on this module) is checked against it by tests/game/ProjectPhysicsApplyTest.
- aver_phys_build_string: Jolt's configuration string plus " aver-phys-state N", UTF-8, NUL-terminated; returns the length needed including the NUL; writes at most cap bytes (always terminated when cap > 0); outUtf8 may be NULL to ask for the length.
- settings block: stored process-wide; defaults are Jolt's own, converted to centimetres; a world that never sets one steps exactly as before; a set applies to the live world at once.
- settings ranges and defaults (name: range, default): VELOCITY_STEPS 2..64, 10; POSITION_STEPS 1..32, 2; COLLISION_STEPS 1..8, 1 (the Update argument); BAUMGARTE 0..1, 0.2; SPECULATIVE_DISTANCE 0.1..20, 2; PENETRATION_SLOP 0.01..10, 2; MAX_PENETRATION 1..200, 20; MANIFOLD_TOLERANCE 0.001..2, 0.1; LINEAR_CAST_THRESHOLD 0.05..1, 0.75; LINEAR_CAST_MAX_PENETRATION 0..1, 0.25; MIN_RESTITUTION_SPEED 0..1000, 100; SLEEP_TIME 0.05..60, 0.5; SLEEP_SPEED 0.1..100, 3; ALLOW_SLEEPING 1; DETERMINISTIC 1; WARM_START 1; BODY_PAIR_CACHE 1; MANIFOLD_REDUCTION 1; LARGE_ISLAND_SPLITTER 1; CHECK_ACTIVE_EDGES 1; MAX_PAIRS_IN_FLIGHT 256..262144, 16384; STEP_LISTENER_BATCH 1..64, 8; STEP_LISTENER_BATCHES_PER_JOB 1..1000000, 1; PAIR_CACHE_DELTA_POS 0.001..10, 0.1; PAIR_CACHE_DELTA_ROT 0.01..30, 2; CONTACT_NORMAL_DELTA 0.1..45, 5; LAMBDA_PRESERVE_DIST 0.01..50, 1.
- settings: LINEAR_CAST_THRESHOLD unit is "fraction of inner radius"; COLLISION_STEPS is "count per fixed step" (the Update argument).
- aver_phys_setting_name: returns NULL if out of range; example name "velocitySteps".
- aver_phys_setting_count: returns AVER_PHYS_WS_COUNT.
- aver_phys_setting_unit: unit strings "count", "cm", "cm/s", "s", "degrees", "fraction", "bool"; NULL if out of range.
- aver_phys_setting_find: case-insensitive; -1 if unknown.
- aver_phys_setting_set: clamps into range; NaN and a bad key are refused (0).
- aver_phys_setting_reset: -1 = every setting.
- aver_phys_setting_get_all: min(n, COUNT) written; returns the count written.
- VELOCITY_STEPS: below 2 friction stops working (Jolt). DETERMINISTIC: off is faster and BREAKS snapshots and replay (kept in code).
- init-time config block: stored as PENDING config, which the next aver_phys_init (and aver_phys_rebuild) reads.
- CF_MAX_BODIES: default 65536, range [1024, 8388607]; Jolt's BodyID limit is 0x7fffff (kept).
- CF_MAX_BODY_PAIRS: default 65536, range [1024, 4194304].
- CF_MAX_CONTACTS: default 16384, range [1024, 1048576].
- CF_TEMP_MB: default 10, range [1, 1024]; contact scratch; Jolt aborts if a frame needs more (kept).
- CF_THREADS: default -1 auto (hardware_concurrency - 2, floor 1); 0 = single-threaded; N = N workers; range [-1, 64].
- aver_phys_config_name: "maxBodies"; NULL if out of range.
- aver_phys_config_set: stores the PENDING value, with or without a world; clamps; NaN refused.
- aver_phys_config_get: the pending value, what the NEXT world will use.
- aver_phys_config_active: what the LIVE world was built with; 0 with no world.
- aver_phys_config_reset: pending back to the default; -1 = every key.
- aver_phys_config_differs: 1 when a world exists and pending != active (the editor's "restart" badge).
- aver_phys_rebuild: shutdown + init with the pending config. With no world it is a plain init. REFUSED (0, reason logged) unless the world is EMPTY: no bodies, characters, joints, ragdolls or vehicles. Settings and shape handles survive; collision-layer matrix, water volumes and every other per-world table do not (they are empty anyway).
- aver_phys_world_empty: 1 when aver_phys_rebuild would be accepted: no world at all, or a world with no bodies, characters, joints, ragdolls or vehicles. The hosts ask this before they rebuild for a project's limits, and the editor's page enables its "Rebuild world" button on it.
- stats block: a snapshot of the last fixed steps, cheap enough for a HUD (kept).
- STEP_MS_LAST: wall time of the last system.Update. STEP_MS_AVG: EMA, 0.5 s time constant (kept). STEP_MS_MAX: window since aver_phys_stats_reset_window (kept).
- CALL_MS_LAST: the whole aver_phys_step call.
- UPDATE_ERRORS: bit set of EPhysicsUpdateError: 1 manifold cache, 2 body pair cache, 4 contact constraints; cumulative since reset (kept).
- WORKER_THREADS: 0 = single-threaded.
- aver_phys_stats: min(n, COUNT) written; returns the count; zeros with no world.
- aver_phys_stats_reset_window: clears the max, the temp peak and the update-error fields; 0 with no world.
- aver_phys_diag_report: the diagnostics preset (AVER_PHYSICS_DIAGNOSTICS). kind 1 broad-phase, 2 per-body simulation, 3 narrow-phase stats, all written to the engine log through Jolt's Trace; 4 Jolt's internal profiler dumped as HTML to the working directory. Returns 1 when produced; 0 in any build without the preset (last_error Unsupported, and a log line), NotInitialised with no world, OutOfRange for another kind. Always exported so the C# parity test sees no conditional ABI.
- aver_phys_diag_log_dir: Jolt's detlog.txt (determinism log, preset AVER_PHYS_BUILD_DETERMINISM_LOG): a directory, or NULL or "" for the working directory. Takes effect when the next world is created (the log is opened then). Returns 1 in the preset, 0 elsewhere.
- profiler block: Jolt's own JPH_PROFILE scopes plus the engine's fixed-step stages, as one tree. Needs AVER_PHYS_BUILD_PROFILER; OFF at run time until enabled (a disabled hook costs one relaxed load and a branch per scope). Rows are published every 60 fixed steps (the first after 5), depth first so a reader indents by depth. Jolt's job scopes run on worker threads and appear under one "Worker jobs" row whose milliseconds are CPU time summed over the workers, not wall time. The table survives a world rebuild; it freezes (is not cleared) when disabled.
- aver_phys_profile_enable: 1 on success; 0 when the build has no hook (last_error Unsupported).
- aver_phys_profile_count: rows in the last published window.
- aver_phys_profile_get: outValues (4 doubles): inclusive ms per fixed step (window average), exclusive ms per step, the largest per-step inclusive ms of any call in the window, calls per step. outParentDepth (2 ints): parent row (-1 = root) and depth. Any out may be NULL; 0 for a bad index.
- aver_phys_profile_summary: "Name 1.2 ms | Name 0.8 ms | Name 0.5 ms" (three largest exclusive scopes of the last window, for a hitch log line). Returns the length needed including the NUL (1 for an empty table); writes at most cap bytes, always terminated when cap > 0; outUtf8 may be NULL.
- AVER_PHYS_BUILD_PROFILER: the external profiler hook is compiled in.


## modules/physics/src/HairInternal.hpp

- file header: the hair registry is shared by PhysicsHairSettings.cpp (settings builders and settings objects) and PhysicsHair.cpp (host install, compute system, instances, update).
- file header: SRC-LOCAL, like PhysicsInternal.hpp: it names JPH:: types. JPH::Hair, HairSettings and HairShaders live inside Aver.Physics, so the module still links Jolt PRIVATE and depends on Core only. The GPU side reaches the compute work only through the function table in physics_hair_abi.h (HairComputeHost.hpp); with no table installed, Jolt's CPU compute backend runs it.
- file header, LOCKING: one mutex guards every table below. Every ABI entry point takes it. settings_finish holds it only for the registry work and runs the quadratic strand split unlocked, so it can sit on a worker thread while the main thread keeps updating other hair. The host table is NOT re-entered from two threads at once: everything that calls into it (buffer creation at finish and create, the queue at update and flush) is under the lock. (Kept in short form: one mutex guards every table; the host table is never entered from two threads at once.)
- HairWindWrapper.cpp: defines AverHairRegisterShaders, which registers the AverHairWind kernel with Jolt's CPU backend, beside Jolt's own HairRegisterShaders.
- HairBuilderRec: "What a settings builder staged, until finish or cancel. Everything is in the ABI's units (cm, engine axes)."
- HairBuilderRec.materials: "by material index, grown on demand".
- HairSettingsRec: "A finished settings object and what the ABI reports about it." instances: "hairs that use it". boundsCm: "min xyz, max xyz in hair space, engine axes" (shortened to "min xyz, max xyz in hair space").
- HairRec: "One hair instance." palette: "jointToHair is identity, so these go to Hair::Update as given". rotation: "hair rotation in Jolt space: rotates the world wind into hair space" (shortened to the same fact, kept). windCB: "the wind kernel's constants".
- HairGlobals.pendingUpdates: "updates recorded since the last flush".
- HairGlobals.compute: created lazily by hairEnsureCompute (needs a world: Jolt's allocator is registered by aver_phys_init) and dropped with the world (kept).
- HairGlobals.windCS: null when the host does not know AverHairWind (wind is then off) (kept).
- HairGlobals.readbackOk: -1 unknown, 0 the host cannot map a readback buffer, 1 it can (kept).
- hairEnsureCompute: creates the compute system, the fifteen shaders plus the wind kernel and the queue if they do not exist yet; the caller holds the mutex and a world exists; false (logged) when the host refused a shader or Jolt's CPU backend lacked one (kept).
- hairDestroyAll: called by aver_phys_shutdown (PhysicsWorld.cpp) before the world goes; safe with no world; destroys builders, settings objects, hairs, then the queue, shaders and compute system, then forgets the installed host (kept).
- the #error in the JPH_USE_CPU_COMPUTE guard is code and was left unchanged.


## modules/physics/src/PhysicsInternal.hpp

- file header: the file is SRC-LOCAL, never include/. Moving it into include/ would silently make the backend part of the module's public contract. modules/physics/CMakeLists.txt links Aver.Physics.Jolt PRIVATE so nothing above this module can include a Jolt header by accident.
- file header, WHY IT EXISTS NOW: World was deliberately file-local before. BuoyancyAbi.cpp's header comment said World is "private to PhysicsWorld.cpp's translation unit" and that findBody is "injected rather than reached for directly". That was right for buoyancy (one lookup, one iteration). It does not hold for the joint, shape and body-dynamics surface, where nearly every function needs bi(), findBody() and the handle counter; injecting three function pointers per file would be ceremony that buys nothing. The invariant that mattered (no Jolt type escapes the module) is unchanged and is enforced by this file's location, not by its contents being unreachable.
- file header: BuoyancyAbi.cpp is deliberately NOT rewritten to use this header. It borrows the public ABI (aver_phys_body_position) to validate a handle; its own comment explains why, and that reasoning still holds for a file that needs exactly one check. Changing it would be churn.
- AVER_PHYS_SCOPE: a named scope in the profiler table (physics_world_abi.h, aver_phys_profile_*), nested under whatever scope is open on this thread. Expands to a Jolt ExternalProfileMeasurement when the hook is built (AVER_PHYSICS_PROFILER) and to nothing otherwise. With the hook built but profiling off, it is a relaxed load and a branch. name must be a string literal.
- Layers, OBJECT LAYER ENCODING: an object layer encodes two things: the broad-phase tree (coarse, structural: static kept apart from moving, so the static half of a level need not be re-inserted every frame) and the set of object layers it collides with. Before this change the module had exactly two layers, NON_MOVING and MOVING, so a caller could not put the player and the player's own bullets on layers that ignore each other. Now an object layer is userLayer * 2 + moving; the broad-phase question is answered by the low bit alone, as before; the collision question is answered by the user layer through a matrix the caller owns. User layer 0 is the default, so a body created without a layer lands on object layer 0 (static) or 1 (moving): the same two numbers as before, with the same broad-phase mapping and pair rule. Existing content is identical, not merely compatible.
- Layers::kUserLayerCount: 16 fits a u16 ObjectLayer at two per user layer with room to spare, and a mask of them fits a u32 for the query filters; more would cost a bigger matrix for layers nobody has asked for.
- Layers::NON_MOVING / MOVING: unchanged in name and value from when they were the only two layers that existed.
- BPLayerInterface: maps each object layer onto its broad-phase layer, which is now the low bit and nothing else.
- ObjectVsBroadPhaseFilter: static geometry only needs testing against things that move; the same rule as before, now asked of the low bit rather than of the whole layer.
- ObjectLayerPairFilter (class comment): the matrix lives inside the filter rather than in a global. Jolt holds a pointer to this object for the life of the PhysicsSystem and asks it from worker threads; keeping the table in the same object means it is created and destroyed with the world, so a matrix set up for one level cannot leak into the next. Reads are unsynchronised, deliberately: a table of bools written between steps and read during them; the alternative is a lock taken on every broad-phase pair.
- ObjectLayerPairFilter::ShouldCollide: static against static is still never tested, whatever the matrix says; a pair that cannot move cannot begin to overlap. The rule was there before and is kept ahead of the caller's matrix so no entry can reintroduce that cost.
- ObjectLayerPairFilter::setCollides: symmetric by construction. Jolt may ask about a pair in either order, and a matrix that disagreed with itself would make collision depend on which body the broad-phase visited first.
- ContactEvent: "Two solid bodies that began touching, and where." subA/subB: "raw Jolt sub-shape ids of body a and b". Kept: SHAPES_MATERIALS (physics_surface_abi.h) filled by fillContactDetail in OnContactAdded and read by aver_phys_contact_detail; surfA/surfB resolved as leaf material, else body label, else 0.
- OverlapEvent: "A body entering or leaving a sensor volume."
- VehicleWheelDesc: "One wheel as the caller described it, in the engine's units." Kept until aver_phys_vehicle_finish builds Jolt's WheelSettingsWV; Jolt wants every wheel at once, the ABI hands them over one call at a time.
- VehicleAxlePair: "One axle as the geometry rule finds it (see vehicleAxles)"; "Left" is the -Y side.
- VehicleAxleDesc: "VEHICLES v2 builder data. Every default is Jolt's own (or the v1 builder's), so a vehicle that never calls a physics_vehicle_ext_abi.h function builds the same Jolt objects v1 did."
- VehicleAxleDesc.diffMode: "1 at 1.4 is Jolt's default, not 'open': see the header" (the referenced header was not named).
- VehicleWheelExt.longSlip group: "empty = Jolt's default curve" (kept).
- VehicleWheelLive: "What the fixed step keeps between steps for one wheel."
- VehicleEntry: "A vehicle: its chassis body and, once finished, the Jolt constraint that drives it." The constraint is also a step listener registered with the system, so it must be removed from both BEFORE the chassis body goes; PhysicsVehicle.cpp's releaseVehicleOfBody is the one place that does it (kept, shortened). body: "the chassis' ordinary body handle".
- VehicleEntry.axleDesc: "grown on demand". VehicleEntry.wheelExt: "grown on demand".
- VehicleEntry.allLongSlip group: "set_wheel_curves(-1)".
- VehicleEntry state banner: "everything below is derived; none of it reads a clock" (kept).
- VehicleEntry.gear: "manual: the requested gear" (kept). shiftLeft: "manual: seconds of open clutch, then of release" (kept).
- VehicleEventRec: "One polled vehicle event (physics_vehicle_ext_abi.h), and an impact before it is merged into one." (split between VehicleEventRec and VehicleImpactRaw).
- TrackingTempAllocator: JPH::TempAllocatorImpl is final, so the high-water mark comes from a wrapper that forwards to one. Allocate and Free are called from several worker threads but always in stack order by job dependency (TempAllocator.h), so there is one logical writer at a time and a relaxed atomic is enough.
- StepStats: "What the stats read; written by statsAfterUpdate / the step-call scope below." stepsThisCall: "fixed steps run so far in the current aver_phys_step call". updateErrors: "OR of EPhysicsUpdateError since the window reset" (kept). updateErrorLogged: "the one log line per world" (kept). workers: "resolved worker threads (0 = single-threaded)" (kept).
- nextWorldId: "A fresh number for every World constructed (PhysicsState.cpp)." The PhysicsState.cpp reference was dropped. A snapshot remembers the one it was taken in, so a handle that outlives its world answers BAD_HANDLE even if a new world has appeared since (kept, shortened).
- World: "The whole simulation: Jolt's system, the handle tables, and the event queues." jobs: "A base pointer: a thread pool, or JobSystemSingleThreaded for PHYSICS.THREADS 0 (PhysicsConfig.cpp)." (the "base pointer" part dropped.)
- World.vehicles: the vehicles share the one handle counter; the chassis is an ordinary entry in bodies; vehicleOfBody is the way back, so aver_phys_remove_body can find the vehicle that must go first without walking the table. An ORDERED map: the vehicle step hooks walk it in handle order, which is their determinism contract. Node-based, so a pointer into an entry (the combine-friction lambda keeps one) survives other inserts.
- World.characterCarry: what each character's GROUND added to its velocity on the last fixed step, in Jolt's units and axes: the moving platform, lift or train it rides. aver_phys_step adds it for the update and takes it back out afterwards, so the velocity a driver reads and writes stays its own. Kept between steps because a character that leaves a moving ground keeps its horizontal part until it lands. Gone with the world; erased with its character, and by a teleport.
- World.characterGravity: aver_phys_character_set_gravity_factor; absent means 1; erased with its character.
- World.byId: "reverse of `bodies`". World.sensors: "which handles are sensors".
- World.surfaceSampled: "wave3: bodies (the terrain) whose contacts take the host's surface friction. Written by aver_phys_body_set_surface_sampled between steps, read by the contact listener on worker threads." (the "wave3:" history tag was dropped.)
- World.vehicleEvents: polled like contacts and cleared at the top of aver_phys_step; the raw impacts are what the contact listener noted (under the mutex) and vehicleStepEnd merges.
- World.stepsTotal: "fixed steps since this world was created".
- World.topologyEpoch/constraintEpoch: topologyEpoch counts every body removal and every constraint add/remove; constraintEpoch only the constraint ones (noteConstraintTopology). A snapshot records both: Jolt restores constraints BY ARRAY INDEX, so a restore into a world whose constraint set changed is refused instead of silently restoring the wrong constraint.
- World.originGeneration: zero in every new World (init, aver_phys_rebuild).
- World LARGE_WORLD group: PhysicsOrigin.cpp, physics_origin_abi.h (kept as the banner).
- g_world: DEFINED in PhysicsWorld.cpp, which owns its lifetime; every other file only reads it.
- noteConstraintTopology: called at every place a Jolt constraint is added to or removed from the system (joint create/remove, vehicle finish/destroy, ragdoll create/destroy): the ONE line each of those files carries for state recording (physics_state_abi.h).
- bi(): UNDEFINED BEHAVIOUR WITH NO WORLD; every caller in this module checks g_world (or resolves a handle) before reaching for it, as the original file-local version required.
- warnBodyLimit: warn-once "body limit reached" message, re-armed by aver_phys_init, defined in PhysicsWorld.cpp.
- addBody: creates a body from a shape, registers it in both handle tables, returns its handle. userLayer DEFAULTS TO 0, which is why every existing call site is unchanged and every body made before user layers existed lands on exactly the object layer it always did.
- findBody: "The Jolt body id behind a handle, or nullptr." findCharacter: "The character behind a handle, or nullptr." writeVec: "Writes a vector into a caller-owned float[3]."
- destroyAllJoints: called by aver_phys_shutdown BEFORE the world goes. A HOOK RATHER THAN A LAZY CLEAR: PhysicsJoints.cpp keeps a table of JPH::Ref<Constraint> that point into the PhysicsSystem and at the bodies inside it. Noticing on the NEXT lookup that g_world had gone would leave that table holding references to constraints of a destroyed system, and aver_phys_joint_count would answer with a previous world's joints until then. Defined in PhysicsJoints.cpp, called from PhysicsWorld.cpp, so the ordering is stated in one place.
- afterStepJoints: breakable joints, checked after every fixed step (kept).
- destroyAllRagdolls / releaseRagdollOfBody: kept.
- destroyAllVehicles: destroys every vehicle's constraint and step listener and empties both vehicle tables. A VehicleConstraint points at a chassis body and into the PhysicsSystem, and is registered with it as a step listener. The chassis bodies themselves are ordinary bodies and go with the rest of the body table. Defined in PhysicsVehicle.cpp.
- releaseVehicleOfBody: if body is a vehicle's chassis, removes the vehicle (constraint and step listener) and forgets it, leaving the body for the caller to remove. Called by aver_phys_remove_body before it touches the body, so a chassis can never be destroyed while a constraint still points at it. A no-op for any other body. Defined in PhysicsVehicle.cpp.
- applyCharacterStairSettings / clearCharacterStairSettings: applies the stair-stepping distances a character was given to the settings the step loop passes to ExtendedUpdate, and forgets them all on shutdown. Both defined in PhysicsCharacter.cpp. Jolt takes the walk-stairs step-up and stick-to-floor step-down as ARGUMENTS to ExtendedUpdate, not as character state, so there is no Jolt-side setter for aver_phys_character_set_stair_stepping to forward to; without this seam the setter would store a value nothing ever reads, the shape this repository keeps shipping and then finding unused.
- vehicleAxles: v1 geometry rule (same x within 5 cm, opposite sides of y = 0), front (largest x) first; the index into this list is the axle number the ABI speaks of (the 5 cm and the front-first order are kept).
- vehicleFillPowertrain: the engine and gearbox settings a vehicle describes, shared by the wheeled and the tracked controller.
- vehicleBuildTracked: builds the WheelSettingsTV wheels and the TrackedVehicleController into settings; false (having logged why) refuses the finish (kept).
- vehicleTrackedInput: the pedals as a tracked controller takes them (forward, left and right track ratios, brake).
- vehicleTrackSpeeds: track speeds at the rim of the driven wheel, cm/s (kept).
- vehicleTrackedBrakeScale: sets every track's brake torque to its builder value times scale.
- vehicleTrackedWheelRpm: average track angular velocity times the final drive and the gearbox ratio, as engine rpm (kept).
- vehicleEngine / vehicleTransmission: either controller's engine and gearbox.
- vehicleApplyInput: sends pedals to the controller of either kind, waking a sleeping chassis on throttle (kept).
- vehicleResetDrivetrain: stops everything that spins and puts the gearbox in neutral (the teleport reset of _set_pose and _flip) (kept).
- vehicleOnContact: called under World::eventMutex on a worker thread; notes a chassis hit for vehicleStepEnd; normal is body a to body b, engine axes (kept).
- stepWorldOnce: one system.Update with the live settings: the collision-step count from the table, the temp allocator and job system the world was built with, the returned EPhysicsUpdateError OR-ed into the stats and logged once per world with the cure named, and the wall time recorded. Replaces the bare Update call in the fixed loop; counts the step in World::stepsTotal.
- StepCallScope: the constructor starts the clock and zeroes the per-call counters; the destructor records steps, contacts and the call's wall time. Does nothing with no world (kept).
- debugStepBegin/End: while a step runs, the lines Jolt draws from inside it (contacts, supporting faces, linear casts, armed by aver_phys_debug_arm) go to a separate buffer that debugStepBegin clears, so after a frame's steps it holds the LAST step's contacts and not a smear of every substep. Both are a single flag test when nothing is armed, and empty in a build without JPH_DEBUG_RENDERER (kept, shortened).
- stateAfterFixedStep: the last statement of every fixed step in aver_phys_step's loop (a shared one-line edit): appends to the hash log when one is open; a single boolean test otherwise.
- diagOnWorldInit / diagOnStepEnd / diagOnWorldShutdown: world creation (makeJobSystem, before the workers start), the end of every fixed step, and the world teardown. Jolt's internal profiler is started and given a frame mark, and the determinism log is opened and closed (PhysicsDeterminism.cpp; empty in every other build).
- stateOnShutdown: drops every snapshot and closes the hash log; called with the world teardown (destroyAllJoints, which aver_phys_shutdown calls first). A snapshot also remembers its World::id, so a stale handle is dead even if this were missed (kept).
- StateParticipant: engine-side state outside this file set (a soft-body registry, a later area's tables) that wants to ride along in a snapshot. save appends the bytes; restore is given exactly those bytes back and returns false if it cannot apply them. name is hashed into the blob, so a participant missing at restore time is skipped rather than misread. Registration is process-wide and may happen at static-init time (kept, shortened).
- jointHandles / clearJointBreakEvents / ragdollHandles: file-static tables in their own .cpp, read by the manifest and cleared by a restore (kept).
- section banners ("---- Layers ----", "---- Vehicles ----", "---- WORLD_STATE_DEBUG ... ----" and similar) were removed; their file references were kept inline where they still matter.


## modules/physics/src/PhysicsOrigin.cpp

- file header: LARGE_WORLD 2.3 (phase reference). The host rebases by delta (engine cm); delta is a whole number of regions (1,638,400 cm = 16,384 m), so the Jolt-space shift is whole metres and exact in f32. A body 8,192 m or more from the origin on a shifted axis loses nothing (Sterbenz); anything within 1 km of the camera lands exactly.
- file header, WHAT MOVES order: (1) joint seam captures what a pulley rebuild needs, (2) every body in World::bodies via BodyInterface::SetPosition with DontActivate (a sleeper stays asleep; an unadded body moves without a broad-phase notify), (3) every character, then RefreshContacts (active contacts hold world positions), (4) joint seam anchors and pulleys, (5) contact, vehicle-impact and vehicle-event points still waiting to be drained, (6) registered hooks.
- file header: a character's inner body is not in World::bodies (Jolt owns it); CharacterVirtual::SetPosition moves it; the body loop skips it by id so it can never move twice.
- file header: a snapshot restore crosses originReshiftRestored; the blob is in the frame it was taken in, so restored items move by the difference of the two origin totals and nothing else moves.
- kOptimizeAboveStatics (4096): more statics moved in one shift and the next step opens with OptimizeBroadPhase; moving many static leaves loosens the tree.
- putHook: a second registration under one name replaces the first in place.
- g_lastWorldId: the world g_last belongs to; a new world reads as "no shift yet".
- Delta.cm / dj: dj is the same delta in Jolt axes and metres; positions change by -dj.
- joltDelta: computed in double so the metres stay whole.
- wholeMetres: rejects non-finite values and values past kMaxDeltaCm.
- parseDelta: Z is never shifted (dz must be 0 and finite).
- shiftPendingEvents: the queues are small and drained between steps; shifting them is simpler than draining.
- moveBody: returns false when the id names no body; isStatic is set when the body is found.
- moveCharacters: SetPosition is used, not aver_phys_character_set_position; all characters move before any refresh so character-against-character contacts see the new frame.
- addCharacter: adds the inner body to the set the body loop skips (Jolt owns the inner body).
- body kinds moved: dynamic, kinematic, static, sensors, heightfields, chassis, ragdoll parts, soft bodies (COM-local vertices ride along).
- aver_phys_origin_shift, hook walk: iterates a copy so a hook that registers another hook cannot invalidate the walk.
- test seam (aver_phys_origin_test_hook): deliberately in no ABI header because tests/abi scans the headers and a function pointer would need a C# mirror nobody wants. PhysicsOriginTest uses it to check hooks run once per shift, in registration order, after the bodies have moved.


## modules/physics/src/PhysicsShapeTable.hpp

- file header: the table is process-wide, mutex-guarded, and has its own counter (a shape handle and a body handle can be the same integer; only the function tells them apart).
- file header: NOT cleared by aver_phys_shutdown. A JPH::Shape is bare geometry, so a handle a caller forgot to release outlives the world, exactly as a mesh shape handle always did.
- file header: SRC-LOCAL like PhysicsInternal.hpp (it names JPH:: types).
- shapeTableAdd: never 0; safe from any thread.
- shapeTableRelease: the shape lives on while a body or another shape uses it.
- shapeTableOwnerFree: true when shape holds no mutable compound, or the one it holds has no live owner.
- MUTABLE compound owner rule: keyed by the shape object, so a second handle on the same shape (aver_phys_shape_of_body) cannot bypass the rule.
- shapeTableBindOwner: records bodyHandle as owner of the mutable compound shape holds; a no-op for any other shape.
- ShapeParams: the SP block of physics_shape_builder_abi.h.
- parseShapeParams: reads n entries; error recorded and reason logged for a block that is too long, non-finite or out of range.
- readJoltQuat: optional engine quaternion (x, y, z, w) as a unit Jolt quaternion; false for non-finite or zero-length.
- allFiniteN: true when all n floats are finite.
- builders: null on failure, with the reason logged and recorded; sizes are engine centimetres; surface 0 = no material.
- HeightfieldParams: the HP block of physics_terrain_abi.h. Defaults are Jolt's (block 2, 8 bits, 5 degrees). An unset field is not passed to Jolt at all, which keeps aver_phys_add_heightfield (an empty block) bit-identical to what it built.
- parseHeightfieldParams: reason logged and recorded for a block too long, non-finite or out of range.
- makeHeightfield: built from cols * rows samples in the v1 physics order (PhysicsTerrain.cpp); NaN, infinite and FLT_MAX samples are holes; the shape is built square on the larger side with the extra samples as holes; cellPalette is (cols-1)*(rows-1) bytes indexing palette (null = none, or palette[0] everywhere when a palette is given); null on failure.
- CompoundChild: position and rotation in the compound's frame (Jolt axes, metres), plus user data.
- makeCompound: kind is AVER_PHYS_COMPOUND_*.
- mutableCompoundOf: the MutableCompoundShape a shape IS (possibly under decorators), or null.
- isMeshLike: a mesh, possibly under decorators.
- shapeMassKg: 0 for a mesh, heightfield or plane.


## modules/physics/src/PhysicsStateRecorder.hpp

- file header: ByteStateRecorder is Jolt's StateRecorder over std::vector<uint8_t> instead of a std::stringstream (StateRecorderImpl): no per-field allocation; capture into a reused vector keeps its capacity, so a ring of snapshots allocates once.
- file header: the same class reads from a pointer and a size, with the usual "attempt to read past the end" EOF and failure flags.
- file header: in validating mode (JPH::StateRecorder::SetValidating) every read compares the stream with the live bytes it is about to overwrite and counts the differences, instead of Jolt's breakpoint.
- file header: ByteWriter / ByteReader are the same thing for the parts Jolt does not write (the header, the manifest, the engine section).
- file header: everything is native little-endian; every platform this engine builds for is little-endian, and a blob from another build is refused by the header's build check long before an endian question could matter.
- ByteWriter::patchU32: overwrites four bytes already written (a length written before its content is known).
- ByteReader::take: a sub-range of n bytes (a length-prefixed section); false when it runs past the end.
- ByteStateRecorder::ReadBytes past the end: Jolt's convention is EOF and failed, and the destination is left as it was.
- ByteStateRecorder write mode appends to `out`; read mode works over a byte range.
- StateManifest: written at the head of every payload. A restore checks it against the live world BEFORE it lets Jolt touch anything (physics_state_abi.h: all-or-nothing), because Jolt's non-validating restore looks each stored body up by id, stops halfway when one is gone, and restores constraints by array index.
- StateManifest::Char::inner: the raw id of the character's inner body (0xFFFFFFFF: none).
- StateManifest: characters and vehicles in handle order; joints and ragdolls ascending; constraintCount is the JPH constraints in the system.
- stateBuildManifest: fills `m` from the live world for a capture with `flags` (AVER_PHYS_SNAP_*).
- stateCheckManifest: changes nothing. Under AVER_PHYS_RESTORE_STRICT in `options` it also fails when the live world holds MORE than `m` does.
- section banner: "what a snapshot expects to find again, and the engine's own section (PhysicsStateEngine.cpp)".
- EngineStage::Wheel f / i: the wheel's telemetry of the LAST step. The next step's traction control and ABS read the slip and the contact flag from them (stepBeginOne), so a restore that left them at their pre-restore values would resimulate differently.
- EngineStage::Veh::vs / vsi: the telemetry a reader sees right after the restore (derived, but cheap).
- stateStageEngine: the section is parsed against the manifest and checked in full before anything is applied.
- stateApplyEngine: cannot fail on its own; a participant that refuses is logged.


## modules/physics/src/SoftBodyInternal.hpp

- file header: shared by PhysicsSoftBody.cpp (the builder), PhysicsSoftBodyExt.cpp (physics_softbody_ext_abi.h) and SoftBodyFluid.cpp (buoyancy), and the hooks that connect them.
- file header: SoftRecord lives here, not in PhysicsSoftBody.cpp, so the Ext translation units can see it.
- file header: a body that never calls an aver_phys_sbx_* function keeps a default SoftExt, which every hook treats as "do nothing" (the one-line header keeps the core of this).
- SoftAttachment::savedInvMass: what the vertex had before it was attached.
- SoftAttachment::maxSpeedCmS: default 1.0e6 (cm/s).
- SoftExt::analysed: measured on first use, not at every rebuild; closed and restVolumeM3 are valid once it is set.
- SoftExt::closed: every face edge shared by exactly two faces.
- SoftExt::group: 0 = none.
- SoftExt::sensorNow / sensorPrev: the sensor set seen this step (filled on worker threads under the event mutex) and last step's.
- SoftRecord::velBefore: vertex velocities at the start of the step, for impacts.
- SoftRecord::palette: the last skin palette (engine matrices), kept for rebuilds.
- registry: records() is ordered, so the loops follow handle order; eventMutex() guards the impact list, the pair slots and the sensor sets; builderVertexCount() is the vertex count a live builder was begun with, 0 when the builder is dead.
- sbxPrepare: appends volumes, LRA, rods and auto constraints, computes rod properties, and adds the one extra face of a face-less body; returns Ok or the refusal reason.
- sbxApply: runs after the body exists and before it is added; applies builder params over the carried values, pressure, skin, group, closed and rest volume, the slot remaps, and the carried attachments.
- sbxEnsureAnalysed: measures closed and the rest volume on first use.
- sbxStepBegin: called with the body write-locked.
- sbxFluidStep (SoftBodyFluid.cpp): hydrostatic pressure and drag on the faces of a body with a fluid density, before the solver.
- section banner "Defined in PhysicsSoftBody.cpp": the registry (records, recordOf, eventMutex, builderVertexCount) is defined there.
- section banner "Hooks PhysicsSoftBody.cpp calls into PhysicsSoftBodyExt.cpp": the sbx* hooks below it are the ones that file implements.
- section banners "the soft-body contact listener" (sbxValidate, sbxContactAdded) and "the fixed loop" (sbxKeepAwake, sbxStepBegin, sbxStepEnd, sbxBeginCall): labels only.
