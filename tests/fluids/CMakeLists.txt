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

# PbfFluidTest -- the particle fluid: solver (tank holds its depth, dam break spreads, scene colliders,
# determinism), the surface mesher, and the desc -> solver-settings mapping. CPU only: PbfFluid.cpp and
# FluidSurface.cpp are pure arithmetic, so this needs no device, no window and no physics world.
add_executable(PbfFluidTest src/PbfFluidTest.cpp)
target_link_libraries(PbfFluidTest PRIVATE Aver.Fluids Aver.RHI Aver.Core Aver.Platform)
set_target_properties(PbfFluidTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# WaterSurfaceTest -- the displaced surface's CPU half: grid layout and builder, camera snapping, and
# the WATER record's cell/radius tokens. No device; links Aver.Formats only for the record parse.
add_executable(WaterSurfaceTest src/WaterSurfaceTest.cpp)
target_link_libraries(WaterSurfaceTest PRIVATE Aver.Fluids Aver.Formats Aver.RHI Aver.Core Aver.Platform)
set_target_properties(WaterSurfaceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# WaterSurfaceCompileTest -- builds water_displace.hlsl's pipeline and a mesh on the adapter (no window,
# no scene), so a shader error fails here instead of in the editor. Skips (77) without a D3D12 device
# or DXC, like VoxiShaderCompileTest.
if(AVER_RHI_D3D12)
  add_executable(WaterSurfaceCompileTest src/WaterSurfaceCompileTest.cpp)
  target_link_libraries(WaterSurfaceCompileTest PRIVATE Aver.Fluids Aver.RHI Aver.RHI.D3D12 Aver.Core Aver.Platform)
  if(AVER_RHI_D3D11)
    target_link_libraries(WaterSurfaceCompileTest PRIVATE Aver.RHI.D3D11)
  endif()
  if(AVER_RHI_VULKAN)
    target_link_libraries(WaterSurfaceCompileTest PRIVATE Aver.RHI.Vulkan)
  endif()
  set_target_properties(WaterSurfaceCompileTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# WaterBodyTest -- the spline, the river and lake meshes and the water point query (wave 3, rivers and
# lakes). CPU only; Aver.RHI for rhi::MeshVertex.
add_executable(WaterBodyTest src/WaterBodyTest.cpp)
target_link_libraries(WaterBodyTest PRIVATE Aver.Fluids Aver.RHI Aver.Core Aver.Platform)
set_target_properties(WaterBodyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# WaterBodyRecordTest -- RIVER / RIVERPT / LAKE / LAKEPT in .ocworld (parse, order, byte-stable round trip).
add_executable(WaterBodyRecordTest src/WaterBodyRecordTest.cpp)
target_link_libraries(WaterBodyRecordTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(WaterBodyRecordTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
