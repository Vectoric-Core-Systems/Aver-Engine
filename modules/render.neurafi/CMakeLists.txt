# Aver.Render.NeuraFI -- frame interpolation (docs/rendering/NEURAFI.md): implementations of
# the aver::rhi::IFrameInterpolator seam declared in Aver.RHI. Milestone 1 is procedural only.
#
# Same narrow link line as Aver.Render.Sr, for the same reason: Aver.Core and the GENERIC Aver.RHI, never
# a backend. The hosts (Sandbox, the runtime) are the composition roots that install it on the device.
aver_add_module(Aver.Render.NeuraFI
  SOURCES
    src/NeuraFI.cpp
  DEPS
    Aver.Core
    Aver.RHI
    Aver.Render.Neural   # the trajectory network (NEURAFI.md §3.5)
)

# HLSL beside the executable -- see cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Render.NeuraFI "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
