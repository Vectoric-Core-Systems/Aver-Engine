# Aver.Camera tests (docs/CAMERAS.md). All headless.
#   CameraRigMathTest   -- the pure math: follow, lag, angles, basis, blends, look-ahead, arm, fade, FOV.
#   CameraModifiersTest -- the modifier stack envelopes, tags, exclusivity, motion gate and cap.
#   CameraRigSystemTest -- the component and system against a real Jolt world through the physics ABI: spring arm,
#                          lag, the six modes, selection and blends, fade, reduce motion, replay equality.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(CameraRigMathTest src/CameraRigMathTest.cpp)
target_link_libraries(CameraRigMathTest PRIVATE Aver.Camera Aver.Core)
set_target_properties(CameraRigMathTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(CameraModifiersTest src/CameraModifiersTest.cpp)
target_link_libraries(CameraModifiersTest PRIVATE Aver.Camera Aver.Core)
set_target_properties(CameraModifiersTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(CameraRigSystemTest src/CameraRigSystemTest.cpp)
target_link_libraries(CameraRigSystemTest PRIVATE Aver.Camera Aver.Physics Aver.Scene Aver.Core)
set_target_properties(CameraRigSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
