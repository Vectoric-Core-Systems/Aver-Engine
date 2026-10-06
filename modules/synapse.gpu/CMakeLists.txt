# Aver.Synapse.Gpu -- the GPU crowd-avoidance backend (see include/aver/synapse/CrowdGpu.hpp).
#
# A separate module so Aver.Synapse stays pure (no RHI): the pure solver defines ICrowdBackend, this
# implements it with a compute shader. Depends on the generic RHI only, never a backend.
aver_add_module(Aver.Synapse.Gpu
  SOURCES
    src/CrowdGpu.cpp
  DEPS
    Aver.Synapse
    Aver.RHI
    Aver.Core
)

target_compile_definitions(Aver.Synapse.Gpu PUBLIC AVER_MODULE_SYNAPSE_GPU=1)

# HLSL beside the executable -- the same deploy stamp render.pcg and occlusion use.
aver_deploy_shaders(Aver.Synapse.Gpu "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
