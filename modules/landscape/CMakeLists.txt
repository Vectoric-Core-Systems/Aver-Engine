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
