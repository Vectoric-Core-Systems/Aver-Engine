# The landscape CPU model. Links no device and no backend, which is the property landscape stages 1
# and 2 exist to establish: the quadtree, the LOD metric, the culling and the chunk geometry are all
# settled before anything tries to draw them.
add_executable(LandscapeTreeTest src/LandscapeTreeTest.cpp)
target_link_libraries(LandscapeTreeTest PRIVATE Aver.Landscape Aver.Formats Aver.Core Aver.Platform)
set_target_properties(LandscapeTreeTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The physics half of the bridge test is END TO END: it builds the converted field in a real Jolt world
# and raycasts where the RENDER geometry says the peak is. That needs Aver.Physics, so it is guarded --
# the index-map assertions still run without it, and the module itself never depends on physics.
#
# A TEST may reach further than its module does. tests/physics already links the Jolt backend directly
# for the same kind of reason: some properties can only be checked from both sides at once.
if(TARGET Aver.Physics)
  target_link_libraries(LandscapeTreeTest PRIVATE Aver.Physics)
  target_compile_definitions(LandscapeTreeTest PRIVATE AVER_LANDSCAPE_TEST_PHYSICS=1)
endif()

# The renderer half is checked with a RECORDING DEVICE rather than a real one -- IDevice's methods are
# virtual no-ops, so a test can be the device, which is the same trick tests/render.ui uses. So this
# still needs no GPU; it needs the RHI's interface, which always exists.
if(TARGET Aver.Landscape.Renderer)
  target_link_libraries(LandscapeTreeTest PRIVATE Aver.Landscape.Renderer)
  target_compile_definitions(LandscapeTreeTest PRIVATE AVER_LANDSCAPE_TEST_RENDERER=1)
endif()
