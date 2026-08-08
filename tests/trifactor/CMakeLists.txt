# ClusterSelectTest -- the virtualized-geometry SELECTION maths (modules/trifactor/include/aver/
# trifactor/ClusterSelect.hpp): screen-space error, frustum culling, backface cone culling, and the
# crack-free LOD cut predicate. Pure CPU: no rhi:: types, no device, no mesh import -- same shape as
# tests/landscape/CMakeLists.txt for the same reason (the decision maths is settled before anything
# tries to draw it).
#
# `if(TARGET Aver.Trifactor)`, not `if(AVER_MODULE_TRIFACTOR)`: an unconditional Aver.Trifactor is a
# hard CMake configure error in a tree where AVER_MODULE_TRIFACTOR is off (modules/trifactor/
# CMakeLists.txt's FATAL_ERROR branch), so this guards on the target actually existing, the same
# pattern tests/formats/CMakeLists.txt already uses for TrifactorTest/ConvertTool.
#
# This links Aver.Trifactor (and therefore, transitively, Aver.Formats and meshoptimizer) even though
# ClusterSelect.hpp/.cpp themselves depend on neither -- see ClusterSelect.hpp's file header for why the
# SOURCE stays decoupled. The link edge exists only because modules/trifactor/CMakeLists.txt is a
# file this test's own workflow may touch in exactly one line (adding src/ClusterSelect.cpp to Aver.
# Trifactor's SOURCES) and no other; there is currently no lighter-weight target that carries just this
# file. The test also links meshoptimizer's own headers directly (see ClusterSelectTest.cpp's own
# header comment) to check the cone-cull sign convention against real meshopt_computeClusterBounds
# output, not just hand-picked numbers.
#
# NOTE ON WIRING (read before assuming this test runs anywhere): there is no tests/CMakeLists.txt in
# this tree for this file to be added from -- every tests/* subdirectory is add_subdirectory()'d
# directly from the ROOT CMakeLists.txt's AVER_BUILD_TESTS block (see e.g. its
# `if(AVER_MODULE_LANDSCAPE) add_subdirectory(tests/landscape) endif()`). Adding this directory's own
# `add_subdirectory(tests/trifactor)` line there is what actually wires this test into the default
# build; that edit was out of scope for the workflow that wrote this file (root CMakeLists.txt was not
# among the files it was allowed to touch) and was left for whoever integrates this slice. Until then
# this CMakeLists.txt is inert but ready -- verified by configuring and building it directly against a
# standalone build directory that add_subdirectory()'s this one folder alongside modules/trifactor.
if(TARGET Aver.Trifactor)
  add_executable(ClusterSelectTest src/ClusterSelectTest.cpp)
  target_link_libraries(ClusterSelectTest PRIVATE Aver.Trifactor Aver.Core meshoptimizer)
  set_target_properties(ClusterSelectTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
