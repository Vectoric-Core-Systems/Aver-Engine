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
