# ParticleSystemTest -- the CPU simulation, with no GPU and no RHI at all.
#
# The failure it exists to catch is this codebase's recurring one (see tests/anim.scene's identical
# header): a component that registers, marshals and stores perfectly and that nothing ever reads. So
# these assertions are about the tick actually spawning, moving and killing particles -- not about
# CParticleEmitter existing.
add_executable(ParticleSystemTest src/ParticleSystemTest.cpp)
target_link_libraries(ParticleSystemTest PRIVATE Aver.Particles Aver.Scene Aver.Core Aver.Platform)
set_target_properties(ParticleSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The wave-10 CPU tests. None creates a device; each is the reference a GPU row is checked against, or the pure half of one.
#   ParticleModulesTest      the RNG's golden values, every module kind, user-parameter binding, curves
#   EmitterSimTest           determinism, budget, ids, shapes, linked ribbons, trails, events
#   ParticleSpawnAtTest      the spawn-at contract (VEHICLES and SOFTBODY_DEPTH call it), the Core seam, the provider
#   ParticleSortNetworkTest  the bitonic network against std::sort
#   ParticleLightingTest     which scene lights a lit emitter takes
foreach(_t ParticleModulesTest EmitterSimTest ParticleSpawnAtTest ParticleSortNetworkTest ParticleLightingTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Particles Aver.Scene Aver.Core Aver.Platform)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ParticleGpuParityTest -- the GPU kernels against EmitterSim on a real device (WARP when asked for D3D12; `vulkan` for the
# Vulkan row, which AVER_CTEST_VULKAN_TARGETS in the root CMakeLists.txt adds). Its own composition root, exactly as
# tests/rhi's ColourVisionShaderTest is and for the same reason. SKIPPED (77) when the backend is absent or reports no
# compute / indirect support.
add_executable(ParticleGpuParityTest src/ParticleGpuParityTest.cpp)
target_link_libraries(ParticleGpuParityTest PRIVATE Aver.Particles Aver.Scene Aver.RHI Aver.Core Aver.Platform)
if(AVER_RHI_D3D12)
  target_link_libraries(ParticleGpuParityTest PRIVATE Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(ParticleGpuParityTest PRIVATE Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(ParticleGpuParityTest PRIVATE Aver.RHI.Vulkan)
endif()
set_target_properties(ParticleGpuParityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
