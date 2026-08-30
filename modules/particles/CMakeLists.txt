# Aver.Particles -- CPU-simulated billboard particles, drawn through the transparent pass
# (modules/rhi/include/aver/rhi/RHIResources.hpp's IRenderFeature::transparentPass). See README.md.
#
# ONE TARGET, not split into a scene-free half and an RHI half the way anim/anim.scene are: unlike
# animation (a pure sampler with a real non-scene consumer -- render.skin's GPU deformation), nothing
# in this engine simulates particles without a scene::CParticleEmitter to drive it, so there is no
# second consumer a split would exist to serve. It DOES depend on Aver.Scene directly, exactly like
# Aver.Anim.Scene does, and for the identical reason -- see the force-off guard on AVER_MODULE_SCENE
# in the root CMakeLists.txt, right beside AVER_MODULE_FRAMEWORK's.
aver_add_module(Aver.Particles
  SOURCES
    src/ParticleEffectLibrary.cpp
    src/ParticleSystem.cpp
    src/ParticleRenderer.cpp
  DEPS
    Aver.Core
    Aver.RHI
    Aver.Scene
  PUBLIC_DEFS
    AVER_MODULE_PARTICLES=1
)

# HLSL beside the executable, redeployed when a shader changes -- the same stamp mechanism
# modules/rhi, render.pbr and render.voxi already use. See cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Particles "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
