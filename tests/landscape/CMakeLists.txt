# The landscape CPU model. Links no device and no backend, which is the property landscape stages 1
# and 2 exist to establish: the quadtree, the LOD metric, the culling and the chunk geometry are all
# settled before anything tries to draw them.
add_executable(LandscapeTreeTest src/LandscapeTreeTest.cpp)
target_link_libraries(LandscapeTreeTest PRIVATE Aver.Landscape Aver.Formats Aver.Core Aver.Platform)
set_target_properties(LandscapeTreeTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
