add_executable(SceneTest src/SceneTest.cpp)
target_link_libraries(SceneTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(SceneTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
