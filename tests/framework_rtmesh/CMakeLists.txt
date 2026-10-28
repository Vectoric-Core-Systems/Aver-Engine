# Runtime meshes, MultiMesh baking and CSG (G4 R7): the pure geometry (aver/formats/MeshBuild.hpp, Csg.hpp) and the C ABI over it
# (framework_rtmesh_abi.h through the Aver.Framework DLL). Headless: no device, no scene, no engine run.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(CsgTest src/CsgTest.cpp)
target_link_libraries(CsgTest PRIVATE Aver.Formats Aver.Platform Aver.Core)
set_target_properties(CsgTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(MeshBuildTest src/MeshBuildTest.cpp)
target_link_libraries(MeshBuildTest PRIVATE Aver.Formats Aver.Platform Aver.Core)
set_target_properties(MeshBuildTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(FrameworkRtMeshAbiTest src/FrameworkRtMeshAbiTest.cpp)
target_link_libraries(FrameworkRtMeshAbiTest PRIVATE Aver.Framework Aver.Formats Aver.Platform Aver.Core)
set_target_properties(FrameworkRtMeshAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
