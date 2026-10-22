# Aver.Render.Hair's tests (docs/plans/wave11/SOFTBODY_HAIR.md section 5). Every target name ends in Test, so the root CMakeLists' recursive registration
# (aver_register_ctest_in) picks them up with the FAIL_REGULAR_EXPRESSION and the skip code every suite here is written against.
#
#   HairRegsTest      HEADLESS, reads files: the register table, the generated headers and the wrappers against Jolt's binding headers.
#   RibbonPackTest    HEADLESS: the ribbon layout and the C++ twin of AverHairRibbons.hlsl.
#   HairMaterialTest  HEADLESS: the Hair material's packing, the library's clamps and the HLSL's flag and model ids.
#   HairSelfTest      HEADED: Jolt's kernels on the GPU compute host against Jolt's CPU backend, on a real device. Exits 77 (skipped) with no device.
#                     `vulkan` runs the Vulkan backend, `hw` the D3D12 adapter instead of WARP, `debug` the validation layer.

# HairRegsTest includes the register table and its row types straight from the module, and reads the vendored Jolt headers from the tree: it links no
# renderer at all.
add_executable(HairRegsTest src/HairRegsTest.cpp)
target_include_directories(HairRegsTest PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/render.hair/include
  ${CMAKE_SOURCE_DIR}/modules/render.hair/src)
target_compile_definitions(HairRegsTest PRIVATE
  AVER_JOLT_SHADER_DIR="${CMAKE_SOURCE_DIR}/modules/physics.jolt/Jolt/Shaders"
  AVER_HAIR_SHADER_DIR="${CMAKE_SOURCE_DIR}/modules/render.hair/shaders"
  AVER_PHYS_HAIR_SHADER_DIR="${CMAKE_SOURCE_DIR}/modules/physics/shaders/hair")
target_link_libraries(HairRegsTest PRIVATE Aver.Core Aver.Platform)
set_target_properties(HairRegsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(RibbonPackTest src/RibbonPackTest.cpp)
target_link_libraries(RibbonPackTest PRIVATE Aver.Render.Hair Aver.RHI Aver.Core Aver.Platform)
set_target_properties(RibbonPackTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(HairMaterialTest src/HairMaterialTest.cpp)
target_compile_definitions(HairMaterialTest PRIVATE AVER_PBR_SHADER_DIR="${CMAKE_SOURCE_DIR}/modules/render.pbr/shaders")
target_link_libraries(HairMaterialTest PRIVATE Aver.Render.PBR.Materials Aver.Render.PBR Aver.Core Aver.Platform)
set_target_properties(HairMaterialTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Links every enabled backend, as the RHI tests do.
add_executable(HairSelfTest src/HairSelfTest.cpp)
target_link_libraries(HairSelfTest PRIVATE Aver.Render.Hair Aver.Physics Aver.RHI Aver.Core Aver.Platform)
if(AVER_RHI_D3D12)
  target_link_libraries(HairSelfTest PRIVATE Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(HairSelfTest PRIVATE Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(HairSelfTest PRIVATE Aver.RHI.Vulkan)
endif()
set_target_properties(HairSelfTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
