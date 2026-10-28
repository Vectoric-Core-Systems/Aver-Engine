# FormatsGodotTest -- the Godot text-format reader (modules/formats.godot) over the hand-written
# fixtures in tests/fabric/data/. Core and Platform only: no device, no .NET, no engine.
# Registered with ctest by the root aver_register_ctest_in walk (it ends in Test).
add_executable(FormatsGodotTest src/FormatsGodotTest.cpp)
target_link_libraries(FormatsGodotTest PRIVATE Aver.Formats.Godot Aver.Platform Aver.Core)
target_compile_definitions(FormatsGodotTest PRIVATE AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FormatsGodotTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GodotMeshTest -- G4 R7: ArrayMesh surfaces decoded from hand-built byte arrays, and GridMap's packed cells and 24 bases (pure data).
add_executable(GodotMeshTest src/GodotMeshTest.cpp)
target_link_libraries(GodotMeshTest PRIVATE Aver.Formats.Godot Aver.Platform Aver.Core)
set_target_properties(GodotMeshTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
