# ParticleSystemTest -- the CPU simulation, with no GPU and no RHI at all.
#
# The failure it exists to catch is this codebase's recurring one (see tests/anim.scene's identical
# header): a component that registers, marshals and stores perfectly and that nothing ever reads. So
# these assertions are about the tick actually spawning, moving and killing particles -- not about
# CParticleEmitter existing.
add_executable(ParticleSystemTest src/ParticleSystemTest.cpp)
target_link_libraries(ParticleSystemTest PRIVATE Aver.Particles Aver.Scene Aver.Core Aver.Platform)
set_target_properties(ParticleSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
