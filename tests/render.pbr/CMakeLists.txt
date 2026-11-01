# CoatEnergyTest -- the layered BSDF's coat lobe, decided by arithmetic rather than by looking.
#
# Links Aver.Core alone. The composition under test is a handful of floats, and the property that
# matters -- a coat must never reflect more light than it receives -- is exactly the kind of claim a
# rendered image cannot settle: a plate that is 3% too bright still looks like a plate.
#
# The white furnace (--furnace-grid) is the GPU-side oracle for the same property and stays the
# confirmation, but it reads TONEMAPPED 8-BIT codes and cannot say WHICH term is wrong when a plate
# comes back bright. This reads raw floats and tests the terms separately.
#
# AVER_REPO_ROOT so it can read the shader it mirrors: the arithmetic here is a C++ port of HLSL, and
# a port that has drifted passes its own tests while the GPU does something else. Same define, and
# the same reason, as tests/render.voxi and tests/repo.
add_executable(CoatEnergyTest src/CoatEnergyTest.cpp)
target_link_libraries(CoatEnergyTest PRIVATE Aver.Core)
target_compile_definitions(CoatEnergyTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(CoatEnergyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# TerrainLayoutTest -- the layered terrain material's CPU/GPU contract (wave 3, landscape layers): the
# HLSL defines against TerrainLayers.hpp, and the parameter texels. Reads the shader like CoatEnergyTest.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(TerrainLayoutTest src/TerrainLayoutTest.cpp)
  target_link_libraries(TerrainLayoutTest PRIVATE Aver.Render.PBR.Materials Aver.Core)
  target_compile_definitions(TerrainLayoutTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(TerrainLayoutTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# MaterialResidencyTest -- phased material residency: stand-ins while textures are pending, swapped in within a per-frame upload
# budget, shared textures uploaded once, failures remembered, prefetch that only queues, and a material destroyed or a device shut
# down mid-load. A recording RHI and a fake async resolver; no GPU.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(MaterialResidencyTest src/MaterialResidencyTest.cpp)
  target_link_libraries(MaterialResidencyTest PRIVATE Aver.Render.PBR.Materials Aver.Core)
  set_target_properties(MaterialResidencyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# TextureLoaderTest -- the background texture loader against real files: queued -> ready -> taken, the derived-data cache hit path
# (the second request compresses nothing), priority order, failure reported once, cancellation, and (the "threads" run) real worker
# threads with a cancel while jobs run. No GPU: a recording factory.
if(TARGET Aver.Assets.Gpu)
  add_executable(TextureLoaderTest src/TextureLoaderTest.cpp)
  target_link_libraries(TextureLoaderTest PRIVATE Aver.Assets.Gpu Aver.Formats Aver.Core)
  set_target_properties(TextureLoaderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  # The plain row (workers off, jobs run by the test) is registered with every *Test target; this is the real-threads row.
  add_test(NAME TextureLoaderThreadsTest COMMAND TextureLoaderTest threads)
  set_tests_properties(TextureLoaderThreadsTest PROPERTIES TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FAIL_REGULAR_EXPRESSION "FAIL  |FAILED ==="
    ENVIRONMENT "AVER_USER_DATA_DIR=${CMAKE_BINARY_DIR}/Testing/userdata/TextureLoaderThreadsTest")
endif()
