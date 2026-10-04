# GerstnerWaveTest -- the wave math, against numbers the test computes for itself.
#
# LINKS Aver.Core ONLY, not Aver.Fluids, and that is a claim rather than a convenience:
# GerstnerWave.hpp is header-only and takes no rhi:: and no device, so if this target ever needs the
# module to link, the separation the module's README argues for has quietly stopped being true.
# NavTest and SynthTest make the same argument the same way.
add_executable(GerstnerWaveTest src/GerstnerWaveTest.cpp)
target_include_directories(GerstnerWaveTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/fluids/include)
target_link_libraries(GerstnerWaveTest PRIVATE Aver.Core Aver.Platform)
set_target_properties(GerstnerWaveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UnderwaterFogTest -- the fog override, which is a pure function over rhi::SkyAtmosphere.
#
# Links Aver.Fluids because applyUnderwaterFog has a real .cpp, and Aver.RHI for the struct it
# transforms -- but still no device, no window and no GPU. That is the whole reason the override was
# written as a function over a plain struct instead of as a method on a render feature: a feature
# would have dragged a device in and made this untestable.
add_executable(UnderwaterFogTest src/UnderwaterFogTest.cpp)
target_link_libraries(UnderwaterFogTest PRIVATE Aver.Fluids Aver.RHI Aver.Core Aver.Platform)
set_target_properties(UnderwaterFogTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FluidVolumeTest -- generateFluidSeedShell's box-lattice geometry and the placement/normal
# bookkeeping FluidVolume layers on top of it.
#
# Same link shape as UnderwaterFogTest, for the same reason: FluidVolume.cpp is a real .cpp (unlike
# header-only GerstnerWave.hpp), so this needs Aver.Fluids proper rather than just its headers, and
# Aver.RHI because that is what Aver.Fluids itself depends on -- but still no device, no window, no
# physics solver anywhere in this target.
add_executable(FluidVolumeTest src/FluidVolumeTest.cpp)
target_link_libraries(FluidVolumeTest PRIVATE Aver.Fluids Aver.RHI Aver.Core Aver.Platform)
set_target_properties(FluidVolumeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
