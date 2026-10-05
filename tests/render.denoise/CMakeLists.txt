# Nrd2ResolveTest -- NRD2's CPU twin (Nrd2ResolveReference: pyramid, resolve forward/backward, oracle
# fit, the spec for nrd2_resolve.hlsli and nrd2_capture.hlsl) and the pose file. Headless: no GPU.
add_executable(Nrd2ResolveTest src/Nrd2ResolveTest.cpp)
target_link_libraries(Nrd2ResolveTest PRIVATE Aver.Render.Denoise Aver.Core)
set_target_properties(Nrd2ResolveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Nrd2TrainerTest -- NRD2 phase 4's CPU pieces (Nrd2Trainer.hpp: standardisation, patch extraction, sampling,
# validation metric, dataset scan, sidecar, gate). Headless: no GPU.
add_executable(Nrd2TrainerTest src/Nrd2TrainerTest.cpp)
target_link_libraries(Nrd2TrainerTest PRIVATE Aver.Render.Denoise Aver.Core)
set_target_properties(Nrd2TrainerTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Nrd2TrainerGpuTest -- the trainer and the network's inference on a real device (WARP by default, `hw`,
# `vulkan`), one standalone submission per frame. Skips (77) without a device; links every enabled backend.
add_executable(Nrd2TrainerGpuTest src/Nrd2TrainerGpuTest.cpp)
target_link_libraries(Nrd2TrainerGpuTest PRIVATE Aver.Render.Denoise Aver.Render.Neural Aver.RHI Aver.Core Aver.Platform)
if(AVER_RHI_D3D12)
  target_link_libraries(Nrd2TrainerGpuTest PRIVATE Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(Nrd2TrainerGpuTest PRIVATE Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(Nrd2TrainerGpuTest PRIVATE Aver.RHI.Vulkan)
endif()
set_target_properties(Nrd2TrainerGpuTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
