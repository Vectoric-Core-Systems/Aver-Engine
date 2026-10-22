# Aver.Render.Hair -- drawing strand hair, and running Jolt's hair simulation on the GPU.
# docs/plans/wave11/SOFTBODY_HAIR.md 2.10, 2.11, 2.16 and docs/HAIR.md.
#
#   RhiComputeHost  the plain-C function table (physics_hair_abi.h, AverPhysComputeHostV1) Aver.Physics' hair simulation runs its compute work
#                   through, implemented over the RHI: it records Jolt's dispatches and replays them into the frame (Jolt's own DX12 and Vulkan
#                   compute backends stay off; the design says why).
#   RibbonPack      the pure half of the draw: the ribbon layout of a groom, its strand-count levels, and the C++ twin of AverHairRibbons.hlsl.
#   HairScene       the render feature: residency per drawn groom, the ribbon expansion pass in prePass, the mesh substitution for the draw walk.
#
# It links the GENERIC RHI and never a backend (a recording device can drive it with no GPU, the property Aver.Render.Skin established), and the
# physics C ABI's HEADERS only: the compute host is handed to Aver.Physics at runtime as a table, so this module never links Jolt.
set(AVER_RENDER_HAIR_SOURCES
  src/RhiComputeHost.cpp
  src/RibbonPack.cpp
  src/HairScene.cpp
)

aver_add_module(Aver.Render.Hair
  SOURCES ${AVER_RENDER_HAIR_SOURCES}
  DEPS
    Aver.Core
    Aver.RHI
    Aver.Scene
    Aver.Formats
    Aver.Hair
    Aver.Hair.Scene
    Aver.Physics
    Aver.Render.PBR
  PUBLIC_DEFS
    AVER_MODULE_RENDER_HAIR=1
)

# The wrappers, the macro header, the generated register headers and the ribbon pass: flat into <exe>/shaders, like every module's.
aver_deploy_shaders(Aver.Render.Hair "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# Jolt's fifteen hair kernels are compiled UNCHANGED from the vendored sources, so the wrappers' #includes need the vendored files beside them.
# Jolt names its headers .h, which aver_deploy_shaders does not copy, so this is its own small copy rule with the same stamp discipline. The
# vendored kernel names (HairIntegrate.hlsl, ...) cannot collide with the wrappers (AverJoltHairIntegrate.hlsl, ...): the engine's shader
# namespace is flat. The wind kernel lives with the physics module (it also compiles there as C++ for the CPU backend) and is copied from it.
set(AVER_JOLT_SHADER_DIR "${CMAKE_SOURCE_DIR}/modules/physics.jolt/Jolt/Shaders")
file(GLOB _av_hair_vendored CONFIGURE_DEPENDS
  "${AVER_JOLT_SHADER_DIR}/Hair*.hlsl"
  "${AVER_JOLT_SHADER_DIR}/Hair*.h"
  "${AVER_JOLT_SHADER_DIR}/Shader*.h")
list(FILTER _av_hair_vendored EXCLUDE REGEX "HairWrapper\.h$")
list(APPEND _av_hair_vendored "${CMAKE_SOURCE_DIR}/modules/physics/shaders/hair/AverHairWind.hlsl")
set(_av_hair_deployed "")
foreach(_av_src IN LISTS _av_hair_vendored)
  get_filename_component(_av_name "${_av_src}" NAME)
  list(APPEND _av_hair_deployed "${CMAKE_BINARY_DIR}/bin/shaders/${_av_name}")
endforeach()
set(_av_hair_stamp "${CMAKE_CURRENT_BINARY_DIR}/Aver.Render.Hair.vendored.stamp")
add_custom_command(
  OUTPUT  ${_av_hair_deployed} "${_av_hair_stamp}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_av_hair_vendored} "${CMAKE_BINARY_DIR}/bin/shaders"
  COMMAND ${CMAKE_COMMAND} -E touch ${_av_hair_deployed} "${_av_hair_stamp}"
  DEPENDS ${_av_hair_vendored}
  COMMENT "Aver.Render.Hair: Jolt's hair kernels and the wind kernel -> bin/shaders"
  VERBATIM)
add_custom_target(Aver.Render.Hair.VendoredShaders DEPENDS ${_av_hair_deployed} "${_av_hair_stamp}")
add_dependencies(Aver.Render.Hair Aver.Render.Hair.VendoredShaders)
set_target_properties(Aver.Render.Hair.VendoredShaders PROPERTIES FOLDER "modules/shaders")
