# GerstnerWaveTest -- the wave math, against numbers the test computes for itself.
#
# LINKS Aver.Core ONLY, not Aver.Water, and that is a claim rather than a convenience:
# GerstnerWave.hpp is header-only and takes no rhi:: and no device, so if this target ever needs the
# module to link, the separation the module's README argues for has quietly stopped being true.
# NavTest and SynthTest make the same argument the same way.
add_executable(GerstnerWaveTest src/GerstnerWaveTest.cpp)
target_include_directories(GerstnerWaveTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/water/include)
target_link_libraries(GerstnerWaveTest PRIVATE Aver.Core Aver.Platform)
set_target_properties(GerstnerWaveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UnderwaterFogTest -- the fog override, which is a pure function over rhi::SkyAtmosphere.
#
# Links Aver.Water because applyUnderwaterFog has a real .cpp, and Aver.RHI for the struct it
# transforms -- but still no device, no window and no GPU. That is the whole reason the override was
# written as a function over a plain struct instead of as a method on a render feature: a feature
# would have dragged a device in and made this untestable.
add_executable(UnderwaterFogTest src/UnderwaterFogTest.cpp)
target_link_libraries(UnderwaterFogTest PRIVATE Aver.Water Aver.RHI Aver.Core Aver.Platform)
set_target_properties(UnderwaterFogTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
