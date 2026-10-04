add_executable(FrameworkTest src/FrameworkTest.cpp)
# Links the gameplay DLL under test AND the scene it stands on (the test drives both surfaces through
# the ONE process-global World), plus Core for the math/hash it asserts against — the tests/scene
# precedent, one module up.
target_link_libraries(FrameworkTest PRIVATE Aver.Framework Aver.Scene Aver.Core)
set_target_properties(FrameworkTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
