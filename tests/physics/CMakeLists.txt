# Links the backend directly, unlike anything else above the physics module: the conversion half of
# the test has to speak BOTH sides to check that converting and rotating commute, which is exactly the
# property a caller of the ABI can never see and therefore can never check.
#
# This is the ONE deliberate exception to "nothing above physics/ knows JPH:: exists", and it is worth
# it being visible on this line rather than buried -- if a second consumer ever appears here, the rule
# has stopped holding.
add_executable(PhysicsTest src/PhysicsTest.cpp)
target_link_libraries(PhysicsTest PRIVATE Aver.Physics Aver.Core Aver.Physics.Jolt)
set_target_properties(PhysicsTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SoftBodyTest -- soft bodies through the plain-C ABI and NOTHING ELSE.
#
# Links Aver.Physics only, deliberately NOT Aver.Physics.Jolt: the comment at the top of this file
# says PhysicsTest is the one exception to "nothing above physics/ knows JPH:: exists", and this
# target is the evidence that the exception is still one target wide. Everything it checks -- a
# cloth hanging from its pinned edge, a body landing on a floor instead of through it, a skinned
# body reproducing ordinary skinning at max-distance 0 -- is visible from outside the ABI, which is
# exactly why it does not need to see inside.
add_executable(SoftBodyTest src/SoftBodyTest.cpp)
target_link_libraries(SoftBodyTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(SoftBodyTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# BuoyancyTest -- the water plane and the per-body override, through the ABI only.
#
# Links Aver.Physics and Aver.Core, NOT Aver.Physics.Jolt, for SoftBodyTest's reason directly above:
# whether a box floats is visible from outside the ABI, so it is checked the way a caller sees it.
#
# THIS TARGET WAS MISSING when the buoyancy code was first written, and the consequence is worth
# recording: the test file existed, compiled by hand, and reported a real failure that nothing in
# the build would ever have run. A test that is not in a CMakeLists is not a test.
# BodyDynamicsTest -- forces, impulses, spin, motion type, material properties, mass, sleeping.
#
# Links Aver.Physics and Aver.Core only, like BuoyancyTest below and for the same reason: every claim
# it makes is about OBSERVED MOTION through the public ABI, so it is checked the way a caller sees it
# rather than by reaching into the module. It needs no Jolt header at all.
# CharacterTest -- and its first test is the one that matters: the stair-stepping distances are
# arguments to Jolt's ExtendedUpdate rather than state on the character, so the setter can only record
# them and something must read them back. Walking a character at a kerb is the only way to tell a
# working feature from a setter that stores a number nothing looks at.
add_executable(CharacterTest src/CharacterTest.cpp)
target_link_libraries(CharacterTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(CharacterTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# VehicleTest -- the wheeled-vehicle ABI, by driving a car on a floor. A vehicle built with Jolt's own
# axis defaults, or with the steering sign the wrong way round, still returns a good handle, so the only
# honest check is observed motion: settle on four wheels, drive along +X, turn toward +Y on +right, stop
# on the brake. Links Aver.Physics and Aver.Core only, like CharacterTest above.
add_executable(VehicleTest src/VehicleTest.cpp)
target_link_libraries(VehicleTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(VehicleTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# VehicleExtTest -- the v2 ABI (physics_vehicle_ext_abi.h): identity with v1 when nothing v2 is called, the torque curve and gearbox,
# differentials, driver aids in the fixed step, telemetry and events. TrackedVehicleTest -- tracked vehicles and the compound chassis.
# Both link Aver.Physics and Aver.Core only, like VehicleTest above.
add_executable(VehicleExtTest src/VehicleExtTest.cpp)
target_link_libraries(VehicleExtTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(VehicleExtTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# HairTest -- strand hair (physics_hair_abi.h): a small groom simulated on Jolt's CPU compute backend and on a FAKE HOST that wraps the same backend behind the C
# function table, which must agree (the adapter proved end to end), plus refusals, droop under gravity, teleport, head motion, a convex hull and wind. THE SECOND
# TARGET HERE THAT LINKS JOLT DIRECTLY: the fake host is a Jolt consumer, and the target compiles HairWindWrapper.cpp itself so its compute system knows the
# AverHairWind kernel. Everything else it checks goes through the ABI.
add_executable(HairTest src/HairTest.cpp ${CMAKE_SOURCE_DIR}/modules/physics/src/HairWindWrapper.cpp)
target_link_libraries(HairTest PRIVATE Aver.Physics Aver.Core Aver.Physics.Jolt)
set_target_properties(HairTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(TrackedVehicleTest src/TrackedVehicleTest.cpp)
target_link_libraries(TrackedVehicleTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(TrackedVehicleTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LayerTest -- collision layers. Its FIRST job is a negative claim: that a project which never
# mentions a layer behaves exactly as it did before layers existed, since every body's object layer
# is now an encoding rather than a constant.
add_executable(LayerTest src/LayerTest.cpp)
target_link_libraries(LayerTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(LayerTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ShapeTest -- the rigid shapes, checked by DROPPING them and measuring where their centres rest.
# That number is a shape's half-height, measured rather than declared, which is the only check that
# tells a capsule built to the right size from one built to twice or half of it.
add_executable(ShapeTest src/ShapeTest.cpp)
target_link_libraries(ShapeTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(ShapeTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# JointTest -- written against physics_joints_abi.h ALONE, by a different author from the
# implementation, deliberately: a test written by reading the implementation agrees with that
# implementation's bugs.
add_executable(JointTest src/JointTest.cpp)
target_link_libraries(JointTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(JointTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(BodyDynamicsTest src/BodyDynamicsTest.cpp)
target_link_libraries(BodyDynamicsTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(BodyDynamicsTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(BuoyancyTest src/BuoyancyTest.cpp)
target_link_libraries(BuoyancyTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(BuoyancyTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FluidDampingCalibrationTest -- measures the mapping from Jolt's raw `damping` solver knob to an
# observable decay time constant, so the fluids module's future viscosity-in-Pa*s knob can be a fit
# against real numbers instead of an invented formula. Same discipline as SoftBodyTest and
# BuoyancyTest above: links Aver.Physics and Aver.Core only, nothing that knows Jolt exists.
add_executable(FluidDampingCalibrationTest src/FluidDampingCalibrationTest.cpp)
target_link_libraries(FluidDampingCalibrationTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(FluidDampingCalibrationTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# RagdollTest -- ragdolls from a skeleton (physics_ragdoll_abi.h).
add_executable(RagdollTest src/RagdollTest.cpp)
target_link_libraries(RagdollTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(RagdollTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# QueryTest -- shape casts, overlaps, multi-hit rays, point queries (physics_queries_abi.h).
add_executable(QueryTest src/QueryTest.cpp)
target_link_libraries(QueryTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(QueryTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# SphereCastQTest -- aver_phys_sphere_cast_q (physics_queries_abi.h): distance, normal, the sensor skip, two ignore slots (a body and a character), layer masks, the refusals.
add_executable(SphereCastQTest src/SphereCastQTest.cpp)
target_link_libraries(SphereCastQTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(SphereCastQTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# JointBreakTest -- breakable joints, soft limits, locked axes, CCD (physics_body_ext_abi.h).
add_executable(JointBreakTest src/JointBreakTest.cpp)
target_link_libraries(JointBreakTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(JointBreakTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CharacterExtTest -- character vs character, inner body, can-stand, contacts.
add_executable(CharacterExtTest src/CharacterExtTest.cpp)
target_link_libraries(CharacterExtTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(CharacterExtTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# WaterSamplerTest / SurfaceFrictionTest -- the host water sampler and the per-surface friction sampler
# (physics_water_abi.h, wave 3 landscape layers). ABI only: Aver.Physics and Aver.Core, like BuoyancyTest.
foreach(_t WaterSamplerTest SurfaceFrictionTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Physics Aver.Core)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# SoftBodyBuilderTest -- the builder-style soft bodies (physics_softbody_abi.h): hanging, rebuilding under the
# same handle with the same state, dents written in place, the bend rule, tears, the skin palette against an
# entity matrix, wind and drag, impacts, layers, sleep and freeze. ABI only: Aver.Physics and Aver.Core.
add_executable(SoftBodyBuilderTest src/SoftBodyBuilderTest.cpp)
target_link_libraries(SoftBodyBuilderTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(SoftBodyBuilderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SoftBodyExtTest -- the rest of Jolt's soft bodies (physics_softbody_ext_abi.h): pressure, tetrahedral volumes, long range attachments,
# Cosserat rods, Jolt's constraint generator, per-vertex inverse mass, grab and release, contact policy, sensors, the collision group,
# skin, buoyancy and determinism. ABI only: Aver.Physics and Aver.Core. (The root CMakeLists registers every *Test executable with ctest.)
add_executable(SoftBodyExtTest src/SoftBodyExtTest.cpp)
target_link_libraries(SoftBodyExtTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(SoftBodyExtTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PhysicsSettingsTest / PhysicsConfigTest -- the world's live solver settings (table, identity, storage, clamping, collision
# steps, sleeping) and its init-time limits (update errors logged once, config/rebuild, threads, body limit, stats), through
# physics_world_abi.h. ABI only: Aver.Physics and Aver.Core. No add_test: the root walk registers every *Test target.
foreach(_t PhysicsSettingsTest PhysicsConfigTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Physics Aver.Core)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ShapeBuilderTest -- the generic shape handles of physics_shape_builder_abi.h (SHAPES_MATERIALS phase A): every leaf rests
# at its half-height, compounds, decorators, set_shape / set_scale, shape info, mesh palettes, dynamic mesh bodies, the old
# creators against the new builders. ABI only, like ShapeTest above: Aver.Physics and Aver.Core.
add_executable(ShapeBuilderTest src/ShapeBuilderTest.cpp)
target_link_libraries(ShapeBuilderTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(ShapeBuilderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SurfaceTest -- physical surfaces (physics_surface_abi.h, SHAPES_MATERIALS phase B): the registry, leaf and per-triangle
# materials changing how far a box slides, restitution, the body label, hit / contact / character-ground / wheel detail, and
# that a project with no surface runs the same bits once one exists. ABI only: Aver.Physics and Aver.Core.
add_executable(SurfaceTest src/SurfaceTest.cpp)
target_link_libraries(SurfaceTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(SurfaceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PhysicsBenchTool -- NOT a test (no `Test` suffix, so the ctest walk skips it). The phase E gate of WORLD_STATE_DEBUG: times building a mesh or
# heightfield shape against restoring it from its blob, on synthetic grids or on real .ocmesh files (--content <project>/Content). Hand-run:
# PhysicsBenchTool shapes. Needs Aver.Formats for the .ocmesh loader.
add_executable(PhysicsBenchTool src/PhysicsBenchTool.cpp)
target_link_libraries(PhysicsBenchTool PRIVATE Aver.Physics Aver.Formats Aver.Core)
set_target_properties(PhysicsBenchTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# TerrainTest -- heightfields v2 (physics_terrain_abi.h, SHAPES_MATERIALS phase C): the legacy creator and the shape route
# agreeing bit for bit, a non-square field, holes, a surface per cell, bits per sample against error, edits (raise, clamp,
# headroom, wake) and the refusals. ABI only: Aver.Physics and Aver.Core.
add_executable(TerrainTest src/TerrainTest.cpp)
target_link_libraries(TerrainTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(TerrainTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PhysicsStateTest / PhysicsDeterminismTest -- state recording (physics_state_abi.h, WORLD_STATE_DEBUG phase D): snapshots, the validate-first
# restore, exact stepping, the state hash, the per-step hash log, thread-count independence. PhysicsShapeCacheTest is phase E's (the shape
# binary cache, same header). ABI only: Aver.Physics and Aver.Core, like the suites above.
foreach(_t PhysicsStateTest PhysicsDeterminismTest PhysicsShapeCacheTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Physics Aver.Core)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# PhysicsOriginTest -- the floating origin's physics half (physics_origin_abi.h, LARGE_WORLD): exactness, sleepers staying asleep, characters, a car, events, refusals, counters.
# ABI only: Aver.Physics and Aver.Core. PhysicsOriginBenchTool is P-0 of the same plan and NOT a test (no `Test` suffix, so the ctest walk skips it): hand-run, it times the shift on
# 10k and 100k statics and 500 sleeping stacks and prints the decision rule of LARGE_WORLD 2.3.7.
add_executable(PhysicsOriginTest src/PhysicsOriginTest.cpp)
target_link_libraries(PhysicsOriginTest PRIVATE Aver.Physics Aver.Core)
set_target_properties(PhysicsOriginTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(PhysicsOriginBenchTool src/PhysicsOriginBenchTool.cpp)
target_link_libraries(PhysicsOriginBenchTool PRIVATE Aver.Physics Aver.Core)
set_target_properties(PhysicsOriginBenchTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# PhysicsProfileTest -- the profiler bridge (physics_world_abi.h, aver_phys_profile_*): the tree of a loaded scene, nesting under Jolt's Update,
# merge by name across threads, frozen when disabled, reset. PhysicsDebugTest -- Jolt's debug renderer as a line feed (physics_debug_abi.h): shape
# extents in engine axes, the radius, the line budget, static triangles near the camera, the last fixed step's contacts, constraints and labels.
# Both skip loudly when the build lacks the hook / the renderer. ABI only: Aver.Physics and Aver.Core.
foreach(_t PhysicsProfileTest PhysicsDebugTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Physics Aver.Core)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
