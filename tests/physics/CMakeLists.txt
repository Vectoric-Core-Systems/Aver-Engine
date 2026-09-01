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
