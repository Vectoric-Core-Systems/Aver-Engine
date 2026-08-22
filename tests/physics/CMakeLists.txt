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
