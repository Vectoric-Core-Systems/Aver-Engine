# Aver.Render.Denoise -- the engine's spatio-temporal denoiser (see README.md). AMD FidelityFX
# Denoiser's reflection pipeline (third_party/fidelityfx-denoiser, MIT) as the filter; this module's
# own C++ and HLSL as the host around it.
#
# RHI-only and unconditional: nothing here is optional at build time. Whether denoising runs is a
# RUNTIME question -- the shaders compile when the renderer starts, and the G-buffer it reads exists
# only on D3D12 -- answered by Denoiser::create() and the caller's own gating.
add_library(Aver.Render.Denoise STATIC
  src/Denoiser.cpp
  src/Nrd2.cpp
  src/Nrd2Capture.cpp
  src/Nrd2Dataset.cpp
  src/Nrd2Network.cpp
  src/Nrd2ResolveReference.cpp
  src/Nrd2Trainer.cpp)

target_include_directories(Aver.Render.Denoise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Denoise PUBLIC cxx_std_20)
# Aver.Render.Neural: CRC-32 and the Adam bias correction (NRD2 dataset and oracle), the NRD2 network.
target_link_libraries(Aver.Render.Denoise PUBLIC Aver.Core Aver.RHI Aver.Render.Neural)
set_target_properties(Aver.Render.Denoise PROPERTIES FOLDER "modules")

# aver_denoise.hlsl beside the executable, like every module's shaders.
aver_deploy_shaders(Aver.Render.Denoise "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
# NRD's trained network (docs/rendering/NEURAA_NRD.md) in bin/data; the renderer points
# Denoiser::setWeightsPath at it. Same deploy rule as NeuraFI's and NeuRAA's weights.
set(_nrd_weights "${CMAKE_CURRENT_SOURCE_DIR}/data/nrd_v1.bin")
set(_nrd_deployed "${CMAKE_BINARY_DIR}/bin/data/nrd_v1.bin")
add_custom_command(
  OUTPUT  ${_nrd_deployed}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/data"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_nrd_weights} "${CMAKE_BINARY_DIR}/bin/data"
  DEPENDS ${_nrd_weights}
  COMMENT "NRD: deploying the trained weights to bin/data")
# NRD2's trained network (docs/rendering/NRD2.md, phase 4) and its .steps sidecar (the live gate reads the
# held-out ratio from it), once they ship; the build works without them (the resolve keeps its defaults).
set(_nrd2_weights "${CMAKE_CURRENT_SOURCE_DIR}/data/nrd2_v1.avnn")
if(EXISTS "${_nrd2_weights}")
  set(_nrd2_files "${_nrd2_weights}")
  if(EXISTS "${_nrd2_weights}.steps")
    list(APPEND _nrd2_files "${_nrd2_weights}.steps")
  endif()
  add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/bin/data/nrd2_v1.avnn"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_nrd2_files} "${CMAKE_BINARY_DIR}/bin/data"
    DEPENDS ${_nrd2_files}
    COMMENT "NRD2: deploying the trained network to bin/data")
  list(APPEND _nrd_deployed "${CMAKE_BINARY_DIR}/bin/data/nrd2_v1.avnn")
endif()
add_custom_target(Aver.Render.Denoise.weights ALL DEPENDS ${_nrd_deployed})
add_dependencies(Aver.Render.Denoise Aver.Render.Denoise.weights)

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
