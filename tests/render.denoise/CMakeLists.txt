# Nrd2ResolveTest -- NRD2's CPU twin (Nrd2ResolveReference: pyramid, resolve forward/backward, oracle
# fit, the spec for nrd2_resolve.hlsli and nrd2_capture.hlsl) and the pose file. Headless: no GPU.
add_executable(Nrd2ResolveTest src/Nrd2ResolveTest.cpp)
target_link_libraries(Nrd2ResolveTest PRIVATE Aver.Render.Denoise Aver.Core)
set_target_properties(Nrd2ResolveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
