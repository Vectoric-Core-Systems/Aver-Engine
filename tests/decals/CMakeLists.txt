# Projected decals (docs/rendering/DECALS.md). Add to the root CMakeLists inside if(AVER_BUILD_TESTS),
# guarded by if(TARGET Aver.Scene AND TARGET Aver.Formats):  add_subdirectory(tests/decals)
#
# No GPU and no window in any of these: the property under test is arithmetic, a component table, a
# pool's bookkeeping and a text format, and a rendered image decides none of them -- a decal 2 cm
# off, a tangent frame mirrored or a pool that leaks a slot all still look like a decal.

# DecalProjectorTest -- the projector maths the shader mirrors, eye-relative packing, paint order, the
# mip chain, and the HLSL source checked against the header's record layout. Links Aver.Core and
# Aver.Platform (to read the HLSL) and reaches the header by include path, as SceneLightTest does.
add_executable(DecalProjectorTest src/DecalProjectorTest.cpp)
target_link_libraries(DecalProjectorTest PRIVATE Aver.Core Aver.Platform)
target_include_directories(DecalProjectorTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(DecalProjectorTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(DecalProjectorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# DecalPoolTest -- CDecal's component table, the gather, lifetimes, and the pool: recycling the
# oldest, no entity churn once warm, expiry, release, clear.
add_executable(DecalPoolTest src/DecalPoolTest.cpp)
target_link_libraries(DecalPoolTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(DecalPoolTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# DecalAbiTest -- the C ABI over the pool (aver_decal_*). A separate executable because it needs
# DecalAbi.cpp in Aver.Scene's source list; DecalPoolTest must not stop building if that line is missing.
add_executable(DecalAbiTest src/DecalAbiTest.cpp)
target_link_libraries(DecalAbiTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(DecalAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# DecalFormatTest -- the .ocworld DECAL record (round trip, defaults omitted, old levels unchanged)
# and the editor's CDecal <-> record conversion (sandbox/src/DecalLevelIo.hpp, header-only).
add_executable(DecalFormatTest src/DecalFormatTest.cpp)
target_link_libraries(DecalFormatTest PRIVATE Aver.Formats Aver.Scene Aver.Core Aver.Platform)
target_include_directories(DecalFormatTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src ${CMAKE_SOURCE_DIR}/modules/world/include
                           ${CMAKE_SOURCE_DIR}/Runtime/include)
set_target_properties(DecalFormatTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# DecalGizmoTest -- the box gizmo's picking and drag arithmetic (sandbox/src/DecalGizmoMath.hpp).
add_executable(DecalGizmoTest src/DecalGizmoTest.cpp)
target_link_libraries(DecalGizmoTest PRIVATE Aver.Scene Aver.Core)
target_include_directories(DecalGizmoTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
set_target_properties(DecalGizmoTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
