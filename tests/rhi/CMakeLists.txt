# AtmosphereTest -- the scattering model with no GPU.
#
# It links Aver.RHI and Aver.Core and nothing else, which is the claim the model makes about itself:
# an atmosphere that needed a device could not be checked against a numeric integral, and every
# property this test asserts is one no screenshot can decide.
add_executable(AtmosphereTest src/AtmosphereTest.cpp)
target_link_libraries(AtmosphereTest PRIVATE Aver.RHI Aver.Core Aver.Platform)
set_target_properties(AtmosphereTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LineMeshTest -- destroyLineMesh's contract, against a real backend.
#
# The only RHI test here that creates a device. It has to: the rules under test (a slot is kept and
# never recycled, a second destroy is refused, a dead handle draws nothing) live in the backends, and
# a mock repeating them would only prove the mock. It asks for WARP, so no GPU is required, and it
# reports SKIPPED rather than failing when neither D3D12 nor Vulkan is present.
add_executable(LineMeshTest src/LineMeshTest.cpp)
target_link_libraries(LineMeshTest PRIVATE Aver.RHI Aver.Core Aver.Platform)

# This test is its OWN composition root, the same way Aver.Runtime is: Aver.RHI calls
# detail::createD3D12Device et al behind AVER_HAS_* but does not link the backends, so a consumer
# has to. Mirroring the guards here rather than linking Aver.Runtime keeps Engine.cpp -- a window,
# a main loop and a settings file -- out of a test about one vector of buffers.
if(AVER_RHI_D3D12)
  target_link_libraries(LineMeshTest PRIVATE Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(LineMeshTest PRIVATE Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(LineMeshTest PRIVATE Aver.RHI.Vulkan)
endif()
set_target_properties(LineMeshTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CbufferLayoutPatchTest -- patchCbuffersForLayout, with no device and no Vulkan headers.
#
# Vulkan-only because the function is Vulkan's (D3D12 needs no such lowering: a root signature says
# which slot is root constants at PIPELINE creation, so DXIL never has to be told). What it checks
# is arithmetic on byte offsets, and the one that matters cannot be checked any other way: a shader
# declaring only b3 must have its fields padded to byte 128, because pushConstantLayout places the
# object block first whether the shader declares it or not. Get that wrong and nothing crashes --
# every vertex is simply skinned by the wrong matrix.
if(AVER_RHI_VULKAN)
  add_executable(CbufferLayoutPatchTest src/CbufferLayoutPatchTest.cpp)
  target_link_libraries(CbufferLayoutPatchTest PRIVATE Aver.RHI.Vulkan Aver.RHI Aver.Core Aver.Platform)
  # Its own composition root, exactly as LineMeshTest above is and for the same reason: Aver.RHI
  # calls detail::createD3D12Device et al behind AVER_HAS_* without linking the backends, so every
  # consumer has to supply them -- even one that never creates a device.
  if(AVER_RHI_D3D12)
    target_link_libraries(CbufferLayoutPatchTest PRIVATE Aver.RHI.D3D12)
  endif()
  if(AVER_RHI_D3D11)
    target_link_libraries(CbufferLayoutPatchTest PRIVATE Aver.RHI.D3D11)
  endif()
  set_target_properties(CbufferLayoutPatchTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# RegisterBindMapTest -- buildRegisterBinds, with no device and no Vulkan headers.
#
# It includes modules/rhi.vulkan/src/VulkanRegisterMap.hpp directly, which is the ONE header in that
# module deliberately kept free of vulkan.h so it can be included from outside (house rule 3 is
# about Vulkan TYPES escaping, and a VkRegisterBind is four integers describing a DXC command-line
# argument). No third_party/vulkan-headers on the include path here, and that is the point.
if(AVER_RHI_VULKAN)
  add_executable(RegisterBindMapTest src/RegisterBindMapTest.cpp)
  target_include_directories(RegisterBindMapTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/rhi.vulkan/src)
  target_link_libraries(RegisterBindMapTest PRIVATE Aver.RHI.Vulkan Aver.RHI Aver.Core Aver.Platform)
  if(AVER_RHI_D3D12)
    target_link_libraries(RegisterBindMapTest PRIVATE Aver.RHI.D3D12)
  endif()
  if(AVER_RHI_D3D11)
    target_link_libraries(RegisterBindMapTest PRIVATE Aver.RHI.D3D11)
  endif()
  set_target_properties(RegisterBindMapTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ShaderCacheSweepTest -- the DXIL blob cache's size bound, with no device and no backend.
#
# Unconditional and Aver.RHI-only: the sweeper is header-only std::filesystem over a temp directory,
# and the property it decides is one no launch can show you. An unbounded cache WORKS -- it just
# grows forever -- so on any machine whose cache has not filled yet, a sweeper that deleted
# everything, nothing, or the newest files instead of the oldest would look identical to a correct
# one. That is exactly the shape of defect this repo has been bitten by before.
add_executable(ShaderCacheSweepTest src/ShaderCacheSweepTest.cpp)
target_link_libraries(ShaderCacheSweepTest PRIVATE Aver.RHI Aver.Core Aver.Platform)
set_target_properties(ShaderCacheSweepTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PrecompiledShaderTest -- ShaderDesc::bytecode and PipelineLayout::constantSpace/samplerSpace,
# together, against a real D3D12 device (WARP, so no GPU required).
#
# Together on purpose: D3D12 validates the shader against the root signature at
# CreateComputePipelineState, so a pipeline that BUILDS is the runtime agreeing that the registers
# were emitted in the spaces the bytecode declares. Neither capability could establish that alone,
# and an assertion written beside the placement code could not establish it at all.
#
# It sources its bytecode from NRD rather than a fixture -- see the file's own comment -- so it
# links Aver.Render.NRD, which exists in every configuration and reports SKIP when NRD is absent.
# D3D12-only because the Vulkan backend deliberately REFUSES a non-zero register space.
if(AVER_RHI_D3D12)
  add_executable(PrecompiledShaderTest src/PrecompiledShaderTest.cpp)
  target_link_libraries(PrecompiledShaderTest PRIVATE Aver.Render.NRD Aver.RHI Aver.RHI.D3D12 Aver.Core Aver.Platform)
  # ITS OWN COMPOSITION ROOT, exactly as LineMeshTest and CbufferLayoutPatchTest above are, and for
  # the same reason: Aver.RHI's createDevice() calls detail::createD3D11Device / createVulkanDevice
  # behind AVER_HAS_*, so every consumer must supply EVERY enabled backend even when it only ever
  # asks for one. Leaving these out links fine right up to LNK2019 on two symbols this test never
  # names -- which is what happened.
  if(AVER_RHI_D3D11)
    target_link_libraries(PrecompiledShaderTest PRIVATE Aver.RHI.D3D11)
  endif()
  if(AVER_RHI_VULKAN)
    target_link_libraries(PrecompiledShaderTest PRIVATE Aver.RHI.Vulkan)
  endif()
  set_target_properties(PrecompiledShaderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ShaderIncludeResolveTest -- can a deployed shader #include a file in a SUBDIRECTORY, and can it do
# it with ANGLED brackets? Against a real D3D12 device (WARP, so no GPU required).
#
# It has to be a runtime test, because HLSL here compiles at runtime: both defects it pins shipped
# as a clean build that failed in the editor, and one of them took every entry point in voxi.hlsl
# down at once -- see the file's own comment.
#
# ITS FIXTURE IS DEPLOYED BY A DIRECTORY COPY, not aver_deploy_shaders, which globs *.hlsl/*.hlsli
# and copies each by NAME alone. That helper flattens, and a flattened fixture would test nothing:
# the whole subject here is that a path survives.
if(AVER_RHI_D3D12)
  add_executable(ShaderIncludeResolveTest src/ShaderIncludeResolveTest.cpp)
  target_link_libraries(ShaderIncludeResolveTest PRIVATE Aver.RHI Aver.RHI.D3D12 Aver.Core Aver.Platform)
  # Its own composition root, exactly as every device-creating test above is: Aver.RHI dispatches to
  # detail::create*Device behind AVER_HAS_* without linking the backends, so a consumer must supply
  # every ENABLED one even when it only ever asks for D3D12.
  if(AVER_RHI_D3D11)
    target_link_libraries(ShaderIncludeResolveTest PRIVATE Aver.RHI.D3D11)
  endif()
  if(AVER_RHI_VULKAN)
    target_link_libraries(ShaderIncludeResolveTest PRIVATE Aver.RHI.Vulkan)
  endif()
  # POST_BUILD rather than a configure-time file(COPY): a fixture that only lands when CMake happens
  # to re-run is a fixture that silently goes stale, and this suite would then be asserting on an
  # old copy of the very thing it is testing.
  add_custom_command(TARGET ShaderIncludeResolveTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/AverIncProbe"
            "${CMAKE_BINARY_DIR}/bin/shaders/AverIncProbe"
    COMMENT "Deploying the AverIncProbe include fixture")
  set_target_properties(ShaderIncludeResolveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
