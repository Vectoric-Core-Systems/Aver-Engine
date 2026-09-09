# Aver.Render.Voxi — optional render-feature module (see README.md).
#
# Built SHARED on purpose: the C# scripting layer P/Invokes this DLL, and keeping the settings
# state in one binary means the editor and the bindings agree on the contract. Depends only on
# Aver.Core, so nothing of the RHI leaks into the DLL.
add_library(Aver.Render.Voxi SHARED
  src/Voxi.cpp)

target_include_directories(Aver.Render.Voxi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Voxi PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.Voxi PUBLIC Aver.Core)

# AVER_VOXI_BUILD flips the export macros to dllexport while building this target.
target_compile_definitions(Aver.Render.Voxi PRIVATE AVER_VOXI_BUILD)
# Consumers get AVER_MODULE_VOXI so they can #if around the whole feature.
target_compile_definitions(Aver.Render.Voxi PUBLIC AVER_MODULE_VOXI=1)

set_target_properties(Aver.Render.Voxi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe

# Aver.Render.Voxi.Renderer — the GPU half of the feature, deliberately a SECOND target.
# Static, because it is linked straight into the host executable, and separate because it needs the
# RHI: giving the DLL above that dependency would put render-hardware types on the P/Invoke boundary.
add_library(Aver.Render.Voxi.Renderer STATIC
  src/VoxiRenderer.cpp
  src/VoxiGiShaders.cpp)

target_include_directories(Aver.Render.Voxi.Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Voxi.Renderer PUBLIC cxx_std_20)
# Aver.RHI is the GENERIC interface. Never Aver.RHI.D3D12: no backend type may cross this boundary,
# and a link line is the only place that rule can actually be enforced.
#
# Aver.Render.PBR.Materials is the architecture on a link line: PBR is a material system, and Voxi
# is the thing that renders it. Voxi supplies visibility and irradiance and calls the shading model;
# the material owns the BRDF. The dependency runs THIS way and never the other -- the material
# system knows nothing about voxel cones.
# Aver.Formats joined this line when the GI derived-data cache did: VoxiRenderer now READS AND
# WRITES a file format (.cache, an AVR1 container), which is a real dependency rather than an
# incidental one and belongs stated here. It is the same shape as the Aver.Render.PBR.Materials
# entry above -- a library this renderer genuinely uses, not a backend leaking across the boundary.
target_link_libraries(Aver.Render.Voxi.Renderer PUBLIC Aver.Core Aver.RHI Aver.Render.Voxi
                                                       Aver.Render.PBR.Materials Aver.Formats)

set_target_properties(Aver.Render.Voxi.Renderer PROPERTIES FOLDER "modules")

# ---- TEMPORARY: isolating shadowPass's cost ------------------------------------------------------
# No CLI flag reaches VoxiRenderer::shadowPass today, and the flag-parsing loop that could add one
# lives in sandbox/src/SandboxApp.cpp -- out of scope for this change. This cache variable is the
# build-time substitute: it caps the cascade count the pass actually loops over (0 disables the
# pass outright), so its cost can be isolated by rebuilding rather than by a run-time switch.
# 4 -- kShadowCascades, the existing cascade count -- is the default and reproduces the unmodified
# renderer exactly: leave this untouched and the #ifdef in VoxiRenderer.cpp never has anything to
# clamp, so nobody who doesn't pass -DAVER_VOXI_SHADOW_CASCADE_LIMIT sees any difference at all.
set(AVER_VOXI_SHADOW_CASCADE_LIMIT 4 CACHE STRING
    "Temporary instrumentation: caps shadowPass's cascade loop to at most this many cascades (0 \
disables the pass entirely, 4 is the unmodified default). For isolating shadowPass's cost; not a \
real quality setting -- do not wire this to the editor or a project manifest.")
target_compile_definitions(Aver.Render.Voxi.Renderer PRIVATE
    AVER_VOXI_SHADOW_CASCADE_LIMIT=${AVER_VOXI_SHADOW_CASCADE_LIMIT})

# HLSL beside the executable. Deployed through a stamp with the .hlsl files as DEPENDS, NOT a
# POST_BUILD command: POST_BUILD runs on a RELINK, and a shader-only edit relinks nothing, so
# the old text stayed deployed and silently kept being compiled. Mirrors modules/rhi/CMakeLists.txt --
# see ShaderFiles.hpp for why bin/shaders is the only path a shipped build looks at.
# ---- RTXDI's HLSL, deployed WITH ITS DIRECTORY STRUCTURE ----
#
# Copied whole into bin/shaders/Rtxdi/... rather than flattened, and the structure is the point.
# <exe>/shaders used to be one flat, case-insensitive basename namespace: RTXDI ships Color.hlsli,
# this engine ships color.hlsli, and deploying both left ONE file on disk -- the engine's colour
# helpers silently gone, caught only by MaterialGraphTest failing on `undeclared identifier
# srgbToLin`, because HLSL compiles at RUNTIME here and none of it was a build error. RTXDI's tree
# also collides with itself (Reservoir.hlsli under DI/, GI/ and PT/). Keeping the directories means
# neither collision can happen; DxcShaderInclude resolves path suffixes now, so the includes RTXDI
# writes against a root ("Rtxdi/GI/Reservoir.hlsli") land on exactly these files.
#
# NOT via aver_deploy_shaders: that flattens into one directory and globs *.hlsl/*.hlsli only, while
# RTXDI ships its shared host/shader structs as .h -- half an include closure, failing at runtime.
# file(COPY) at configure time preserves both the tree and the extensions.
file(COPY "${CMAKE_SOURCE_DIR}/third_party/rtxdi/Include/Rtxdi"
     DESTINATION "${CMAKE_BINARY_DIR}/bin/shaders")

aver_deploy_shaders(Aver.Render.Voxi.Renderer "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
