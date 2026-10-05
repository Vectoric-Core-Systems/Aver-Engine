# NeuralMlpTest -- Aver.Render.Neural's CPU reference, headless: no GPU, no device, no backend.
#
# The GPU network cannot be tested without a device, so the maths it must reproduce lives in
# MlpReference (the spec for aver_neural_mlp.hlsl) and the properties are checked there: the
# backward pass against finite differences, a toy function being learned, the init and the weight
# file, and the order-independence of the fixed-point gradient accumulation.
add_executable(NeuralMlpTest src/NeuralMlpTest.cpp)
target_link_libraries(NeuralMlpTest PRIVATE Aver.Render.Neural Aver.Core)
set_target_properties(NeuralMlpTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NeuralConvTest -- the convolution layers' CPU reference (ConvNetReference, the spec for
# aver_neural_conv.hlsl), headless like NeuralMlpTest.
add_executable(NeuralConvTest src/NeuralConvTest.cpp)
target_link_libraries(NeuralConvTest PRIVATE Aver.Render.Neural Aver.Core)
set_target_properties(NeuralConvTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NeuralGpuParityTest -- the GPU kernels against their CPU twins on a real device (WARP by default,
# `hw` for the adapter, `vulkan` for the Vulkan row). Skips (77) when the backend or
# IDevice::runStandaloneCompute is unavailable. Links every enabled backend, as the RHI tests do.
add_executable(NeuralGpuParityTest src/NeuralGpuParityTest.cpp)
target_link_libraries(NeuralGpuParityTest PRIVATE Aver.Render.Neural Aver.RHI Aver.Core Aver.Platform)
if(AVER_RHI_D3D12)
  target_link_libraries(NeuralGpuParityTest PRIVATE Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(NeuralGpuParityTest PRIVATE Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(NeuralGpuParityTest PRIVATE Aver.RHI.Vulkan)
endif()
set_target_properties(NeuralGpuParityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
