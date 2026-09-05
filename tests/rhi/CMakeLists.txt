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
