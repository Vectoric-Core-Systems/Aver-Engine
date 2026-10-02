# Aver.Render.Denoise -- the engine's spatio-temporal denoiser (see README.md). AMD FidelityFX
# Denoiser's reflection pipeline (third_party/fidelityfx-denoiser, MIT) as the filter; this module's
# own C++ and HLSL as the host around it.
#
# RHI-only and unconditional: nothing here is optional at build time. Whether denoising runs is a
# RUNTIME question -- the shaders compile when the renderer starts, and the G-buffer it reads exists
# only on D3D12 -- answered by Denoiser::create() and the caller's own gating.
add_library(Aver.Render.Denoise STATIC
  src/Denoiser.cpp)

target_include_directories(Aver.Render.Denoise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Denoise PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.Denoise PUBLIC Aver.Core Aver.RHI)
set_target_properties(Aver.Render.Denoise PROPERTIES FOLDER "modules")

# aver_denoise.hlsl beside the executable, like every module's shaders.
aver_deploy_shaders(Aver.Render.Denoise "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# ---- FidelityFX Denoiser's headers, deployed under bin/shaders/FidelityFX ----
# They are .h files, which aver_deploy_shaders does not carry (it globs .hlsl/.hlsli and flattens),
# and they belong in their own directory: <exe>/shaders is a flat, case-insensitive namespace on
# Windows, and a vendored tree there can silently overwrite an engine file of the same name.
# aver_denoise.hlsl includes them as "FidelityFX/<name>.h", and DxcShaderInclude resolves the
# headers' own sibling includes against that directory. Their MIT licence text travels with them,
# since bin/shaders is what a staged build ships.
file(GLOB _av_ffx_dnsr_headers "${CMAKE_SOURCE_DIR}/third_party/fidelityfx-denoiser/*.h")
file(COPY ${_av_ffx_dnsr_headers} "${CMAKE_SOURCE_DIR}/third_party/fidelityfx-denoiser/LICENSE.txt"
     DESTINATION "${CMAKE_BINARY_DIR}/bin/shaders/FidelityFX")
