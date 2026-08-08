# meshoptimizer (MIT) — vendored, sources only. Third-party: warnings silenced.
#
# Sources are listed EXPLICITLY rather than globbed, matching third_party/imgui. A glob would pick
# up whatever a future re-vendor happens to drop in the directory without anyone reviewing it, and
# the point of vendoring narrowly is that the file list is a decision, not an accident.
add_library(meshoptimizer STATIC
  src/allocator.cpp
  src/clusterizer.cpp
  src/indexanalyzer.cpp
  src/indexcodec.cpp
  src/indexgenerator.cpp
  src/meshletcodec.cpp
  src/meshletutils.cpp
  src/opacitymap.cpp
  src/overdrawoptimizer.cpp
  src/partition.cpp
  src/quantization.cpp
  src/rasterizer.cpp
  src/simplifier.cpp
  src/spatialorder.cpp
  src/stripifier.cpp
  src/tangentspace.cpp
  src/vcacheoptimizer.cpp
  src/vertexcodec.cpp
  src/vertexfilter.cpp
  src/vfetchoptimizer.cpp
)
target_include_directories(meshoptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(meshoptimizer PRIVATE /w)
endif()
set_target_properties(meshoptimizer PROPERTIES FOLDER "third_party")
