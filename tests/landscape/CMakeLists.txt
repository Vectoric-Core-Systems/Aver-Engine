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

  # LandscapeEvictTest -- whether the mesh cache RECLAIMS residency or only refuses to grow.
  #
  # A separate executable rather than another section of the one above, because it asks a different
  # kind of question: not "is the geometry right" but "is the bookkeeping conserved". Its counting
  # device turns a GPU memory leak -- previously visible only in a profiler -- into the arithmetic
  # property `destroyed + live == created`, which needs no GPU at all.
  add_executable(LandscapeEvictTest src/LandscapeEvictTest.cpp)
  target_link_libraries(LandscapeEvictTest PRIVATE
    Aver.Landscape.Renderer Aver.Landscape Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(LandscapeEvictTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# SculptTest -- the sculpt editor's CPU model: brush edits (raise/lower/smooth/flatten), the
# ray-vs-heightfield pick a cursor needs, and that editing a section invalidates exactly the resident
# meshes the edit could have touched, not the whole cache. Aver.Landscape only; the renderer-cache
# section is guarded the same way LandscapeTreeTest's own renderer section is, for the same reason.
add_executable(SculptTest src/SculptTest.cpp)
target_link_libraries(SculptTest PRIVATE Aver.Landscape Aver.Formats Aver.Core Aver.Platform)
set_target_properties(SculptTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(TARGET Aver.Landscape.Renderer)
  target_link_libraries(SculptTest PRIVATE Aver.Landscape.Renderer)
  target_compile_definitions(SculptTest PRIVATE AVER_LANDSCAPE_TEST_RENDERER=1)
endif()

# TerrainGenTool -- not a test. Writes an .ocland from fBm noise, for the same reason LevelInspect
# and ScatterBudgetTool exist: it answers a real need that a fixture cannot. Aver.Landscape shipped a
# complete heightfield runtime and this repository contained no .ocland file at all, so there was
# nothing to render, sculpt, collide with, or scatter onto -- the sculpt tools can reshape a section
# but cannot conjure the first one.
#
#     TerrainGenTool.exe <out.ocland> [samples] [spacingCm] [amplitudeCm] [seed] [featureCm]
#
# Formats only: the noise is self-contained (see the file's header for why it does not borrow
# render.pcg's), so this needs neither Aver.Landscape nor a device.
add_executable(TerrainGenTool src/TerrainGenTool.cpp)
target_link_libraries(TerrainGenTool PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(TerrainGenTool PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
