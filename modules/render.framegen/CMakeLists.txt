# Aver.Render.FrameGen -- frame interpolation (docs/rendering/FRAME_INTERPOLATION.md): implementations of
# the aver::rhi::IFrameGenerator seam declared in Aver.RHI. Milestone 1 is procedural only.
#
# Same narrow link line as Aver.Render.Sr, for the same reason: Aver.Core and the GENERIC Aver.RHI, never
# a backend. The hosts (Sandbox, the runtime) are the composition roots that install it on the device.
aver_add_module(Aver.Render.FrameGen
  SOURCES
    src/ProceduralFrameGenerator.cpp
  DEPS
    Aver.Core
    Aver.RHI
)

# HLSL beside the executable -- see cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Render.FrameGen "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
