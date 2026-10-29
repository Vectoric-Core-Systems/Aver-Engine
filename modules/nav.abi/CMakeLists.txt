# Aver.Nav.Abi -- the plain-C seam onto polygon navigation (Godot NavigationServer3D parity), the navmesh baker and the A* graphs, so
# Aver.Fabric drives them.
#
# SHARED because the .NET host P/Invokes it. It links Aver.Synapse (pure: no scene, no physics, no device) statically and nothing else, so a
# managed test host loads it from a plain folder. The file is Aver.Nav.Abi and no managed assembly shares the name, so the shared
# NativeResolver that finds Aver.* beside the executable is all it needs.
add_library(Aver.Nav.Abi SHARED src/NavAbi.cpp)

target_include_directories(Aver.Nav.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Nav.Abi PUBLIC cxx_std_20)
target_link_libraries(Aver.Nav.Abi PRIVATE Aver.Synapse Aver.Formats Aver.Platform Aver.Core)
target_compile_definitions(Aver.Nav.Abi PRIVATE AVER_NAV_ABI_BUILD)

set_target_properties(Aver.Nav.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # beside the exe, where DllImport looks
