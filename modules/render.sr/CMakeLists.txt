# Aver.Render.Sr — AverSR, the engine's upscaling subsystem (docs/AVERSR.md). Implementations of
# the aver::rhi::IUpscaler seam declared in Aver.RHI: today, the built-in spatial (Catmull-Rom)
# resample; later, AMD FSR registered alongside it without this target's public API changing.
#
# STATIC, and deliberately the narrowest link line in the tree: Aver.Core and Aver.RHI, nothing
# else. Held to the standard modules/render.voxi already meets -- Voxi is the reference because it
# is the module that most obviously could have been coupled (materials, the sun, GI) and is not.
# AverSR must never depend on Voxi, PBR, Scene, World, Landscape, Physics or Trifactor: an upscaler
# resamples a colour target and has no legitimate reason to know what a material, an entity or a
# chunk is. See docs/AVERSR.md "Module boundaries" for the full argument and the enforcement list.
#
# Aver.RHI here is the GENERIC interface -- never Aver.RHI.D3D12, never Aver.RHI.Vulkan. No backend
# type may cross this boundary, and this link line is the only place that rule is actually enforced
# rather than merely intended.
aver_add_module(Aver.Render.Sr
  SOURCES
    src/AverSrSpatial.cpp
    src/AverSrFsr.cpp
  DEPS
    Aver.Core
    Aver.RHI
  PUBLIC_DEFS
    AVER_MODULE_SR=1
)

# HLSL beside the executable -- see cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Render.Sr "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
# FSR 1's headers, which sr_fsr1.hlsl includes as "FidelityFX/<name>.h" (the denoiser's live there too).
file(COPY "${CMAKE_SOURCE_DIR}/third_party/fidelityfx-fsr/ffx_a.h"
          "${CMAKE_SOURCE_DIR}/third_party/fidelityfx-fsr/ffx_fsr1.h"
     DESTINATION "${CMAKE_BINARY_DIR}/bin/shaders/FidelityFX")
