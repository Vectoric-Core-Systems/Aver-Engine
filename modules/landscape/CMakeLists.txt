# Aver.Landscape -- the CPU half of the landscape: samples, the quadtree, LOD selection, culling and
# chunk geometry (see README.md).
#
# STATIC and with NO RHI DEPENDENCY, and both are the point rather than incidental. Static because it
# is linked straight into whatever draws it and there is no C seam here yet; RHI-free because that is
# what lets every number in it be checked headlessly -- the tests link this and Aver.Formats and run on
# a machine with no GPU, no device and no .NET.
#
# The renderer half is a SEPARATE target for the reason Aver.Render.Voxi.Renderer is one: it needs the
# RHI, and giving this target that dependency would end the property above. It is not added yet.
add_library(Aver.Landscape STATIC
  src/LandscapeTree.cpp
  src/ChunkMesh.cpp
  # The physics sample adapter. Emits a plain array, so this stays free of Aver.Physics.
  src/PhysicsBridge.cpp)

target_include_directories(Aver.Landscape PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Landscape PUBLIC cxx_std_20)
# Aver.Formats for OcLandData, and nothing else. Deliberately not Aver.Assets yet: nothing here loads
# by asset id, and a dependency added before it is used is a dependency nobody can later justify.
target_link_libraries(Aver.Landscape PUBLIC Aver.Core Aver.Formats)

target_compile_definitions(Aver.Landscape PUBLIC AVER_MODULE_LANDSCAPE=1)

set_target_properties(Aver.Landscape PROPERTIES FOLDER "modules")

# Aver.Landscape.Renderer -- the GPU half, and a SECOND target for the reason
# Aver.Render.Voxi.Renderer is one: it needs the RHI, and giving the target above that dependency
# would end the property that makes the CPU model testable with no device.
#
# Aver.RHI is the GENERIC interface. Never Aver.RHI.D3D12: no backend type may cross this boundary, and
# a link line is the only place that rule can actually be enforced.
add_library(Aver.Landscape.Renderer STATIC
  src/LandscapeRenderer.cpp)

target_include_directories(Aver.Landscape.Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Landscape.Renderer PUBLIC cxx_std_20)
target_link_libraries(Aver.Landscape.Renderer PUBLIC Aver.Core Aver.RHI Aver.Landscape)

set_target_properties(Aver.Landscape.Renderer PROPERTIES FOLDER "modules")
