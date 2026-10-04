# Aver.Render.NeuraFI -- NeuraFI, Neural Frame Interpolation (docs/rendering/NEURAFI.md): the
# aver::rhi::IFrameInterpolator the hosts install. The procedural gather plus the learned trajectory path.
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

# THE TRAINED NETWORK THE ENGINE SHIPS WITH, beside the executable in bin/data (NeuraFI::
# setShippedWeightsPath). A user's own training writes to their user-data folder and wins over this; the
# ".steps" sidecar here carries only the lifetime step count, no gate verdict, so every user's live gate
# judges the network on their own motion before it is used. Both packaging allowlists ship data/**.
# Same "the deployed files are the output" rule as aver_deploy_shaders: deleting one from bin/data is
# noticed by the next build.
set(_neurafi_weights
  "${CMAKE_CURRENT_SOURCE_DIR}/data/neurafi_v2.avnn"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/neurafi_v2.avnn.steps")
set(_neurafi_deployed
  "${CMAKE_BINARY_DIR}/bin/data/neurafi_v2.avnn"
  "${CMAKE_BINARY_DIR}/bin/data/neurafi_v2.avnn.steps")
add_custom_command(
  OUTPUT  ${_neurafi_deployed}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/data"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_neurafi_weights} "${CMAKE_BINARY_DIR}/bin/data"
  DEPENDS ${_neurafi_weights}
  COMMENT "NeuraFI: deploying the trained weights to bin/data")
add_custom_target(Aver.Render.NeuraFI.weights ALL DEPENDS ${_neurafi_deployed})
add_dependencies(Aver.Render.NeuraFI Aver.Render.NeuraFI.weights)
