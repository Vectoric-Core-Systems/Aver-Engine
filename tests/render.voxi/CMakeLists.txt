# VoxiRtSeqTest -- the sun-disc sample sequence, with no GPU.
#
# It links Aver.Core and nothing else. The property under test is arithmetic -- that a sample's
# position does not depend on how many samples are being taken -- and arithmetic is exactly what a
# rendered image cannot decide: a shadow sampled by a completely different set of rays still looks
# like a shadow. The HLSL it asserts against is read from modules/render.voxi/shaders/voxi.hlsl.
add_executable(VoxiRtSeqTest src/VoxiRtSeqTest.cpp)
target_link_libraries(VoxiRtSeqTest PRIVATE Aver.Core)
# AVER_REPO_ROOT, not an include path: the HLSL this asserts against is a FILE now
# (modules/render.voxi/shaders/voxi.hlsl), not a raw string literal in a header, so the test reads it
# rather than compiling it in. Same define tests/repo and tests/editor already use to reach the tree.
target_compile_definitions(VoxiRtSeqTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(VoxiRtSeqTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# VoxiHemiSampleTest -- the lighting-contrast fix's F1 sampler (root cause R0): a cosine-weighted
# hemisphere sample checked against the fixed 45-degree ring it replaces. Same shape as
# VoxiRtSeqTest above: links Aver.Core only, no GPU, source assertions read the actual HLSL text
# through AVER_REPO_ROOT.
add_executable(VoxiHemiSampleTest src/VoxiHemiSampleTest.cpp)
target_link_libraries(VoxiHemiSampleTest PRIVATE Aver.Core)
target_compile_definitions(VoxiHemiSampleTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(VoxiHemiSampleTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ConeWeightTest -- the lighting-contrast fix's F5 cone weights (root cause R6): the voxel cone
# gather's directions checked against the true cosine-hemisphere moments, and coneTracedIndirect's
# body checked byte-identical between voxi_cone.hlsli and voxi_gi.hlsli. Same linkage as the two
# tests above.
add_executable(ConeWeightTest src/ConeWeightTest.cpp)
target_link_libraries(ConeWeightTest PRIVATE Aver.Core)
target_compile_definitions(ConeWeightTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(ConeWeightTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SkyOwnershipIdentityTest -- the lighting-contrast fix's F4 sky-ownership identity (root cause R1):
# an algebraic identity over material_prelude.hlsl's own diffAmbient/diffBounce formulas, checked
# against 10k random tuples, plus the source strings F4 (voxi.hlsl) and F6 (pt_pathtrace.hlsl) land.
# Same linkage as the tests above.
add_executable(SkyOwnershipIdentityTest src/SkyOwnershipIdentityTest.cpp)
target_link_libraries(SkyOwnershipIdentityTest PRIVATE Aver.Core)
target_compile_definitions(SkyOwnershipIdentityTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(SkyOwnershipIdentityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GiCacheTest -- the .cache derived-data entry a GI volume is baked into.
#
# Links Aver.Formats and nothing renderer-shaped, because the half worth testing without a GPU is
# the half that decides whether a HIT is safe: mip arithmetic, an exact round trip, a key that is
# a miss when it differs anywhere at all, and a truncated or mislabelled file being rejected rather
# than half-read into a volume.
add_executable(GiCacheTest src/GiCacheTest.cpp)
target_link_libraries(GiCacheTest PRIVATE Aver.Formats Aver.Assets Aver.Core Aver.Platform)
set_target_properties(GiCacheTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CameraFactorTest -- recovering the separate worldToView/viewToClip from the combined viewProj
# VoxiRenderer already carries everywhere else, checked against real cameras built from
# Mat4::lookAtLH/perspectiveLH.
#
# Links Aver.Core and nothing else, same as VoxiRtSeqTest above and for the same reason: the header
# under test (aver/voxi/CameraFactor.hpp) depends on nothing but aver/core/Types.hpp by design (see
# its own top comment), and this test only additionally needs Mat4/Vec3 -- from Aver.Core already --
# to BUILD the cameras it hands the header. No RHI, no device, no GPU: the property is a closed-form
# recovery of one 4x4 into two, decidable on the CPU alone.
add_executable(CameraFactorTest src/CameraFactorTest.cpp)
target_link_libraries(CameraFactorTest PRIVATE Aver.Core)
# CameraFactor.hpp lives under Aver.Render.Voxi's include tree, but this test deliberately does not
# link that target (or anything RHI-shaped) -- it only needs the one header, so it reaches it with an
# include path rather than a library dependency that would pull in machinery this test never uses.
target_include_directories(CameraFactorTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
set_target_properties(CameraFactorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GiDispatchBoundsTest -- W3's occupied-bounds arithmetic (aver/voxi/GiDispatchBounds.hpp): the box a
# bounded GI rebuild confines CSClear/CSResolve/CSMip to, and the C++ mirror of voxi.hlsl's MipCB
# cbuffer. Same shape as CameraFactorTest just above: the header depends on nothing but
# aver/core/Types.hpp by design (see its own top comment), so this links Aver.Core alone and reaches
# the header with an include path rather than a library dependency. AVER_REPO_ROOT, for the same
# reason VoxiRtSeqTest above needs it: the final section reads modules/render.voxi/shaders/voxi.hlsl
# itself to check the MipCB declaration and the CSClear/CSResolve/CSMip guard line are still the exact
# text this header's callers depend on.
add_executable(GiDispatchBoundsTest src/GiDispatchBoundsTest.cpp)
target_link_libraries(GiDispatchBoundsTest PRIVATE Aver.Core)
target_include_directories(GiDispatchBoundsTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(GiDispatchBoundsTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(GiDispatchBoundsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GiVisibilityTest -- U1's shared bit definitions and reconstruction arithmetic
# (aver/voxi/GiVisibility.hpp): the giRestirVisibility mode/half-res-pair packing gAmbientParams.w
# carries, the tracedPixel phase schedule, and the reconstruction weight giVisReconstruct sums per
# tap. Same shape as GiDispatchBoundsTest just above: the header depends on nothing but
# aver/core/Types.hpp by design, so this links Aver.Core alone and reaches the header with an include
# path rather than a library dependency. AVER_REPO_ROOT, for the same reason GiDispatchBoundsTest
# needs it: the source-assertion section reads modules/render.voxi/shaders/voxi_restir.hlsli itself
# to check the AVER_GI_VIS_* literals, the gAverHistoryWrite gate count, and the decode-block text
# this header's packAmbientW/tracedPixel mirror are still the exact text the wave-2 contract specifies.
add_executable(GiVisibilityTest src/GiVisibilityTest.cpp)
target_link_libraries(GiVisibilityTest PRIVATE Aver.Core)
target_include_directories(GiVisibilityTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(GiVisibilityTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(GiVisibilityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NeuRaCTest -- the radiance cache's CPU-checkable half (aver/voxi/NeuRaCLayout.hpp): the
# fixed-point headroom, the accumulator and RcInfo layout, tag/cell addressing (negative cells included),
# origin snapping, the fp16 SH and the normal word (direction, length, trust), the SH cosine convolution
# against a Monte Carlo estimate with the scatter's own weighting, the scatter binning (the wall case),
# the accept-rate and cap tables, the lookup corner weight, and the AVER_RC_* #define literals in
# voxi_neurac.hlsli against the header's constants. Same shape as GiVisibilityTest above: the header depends on nothing but
# aver/core/Types.hpp, so this links Aver.Core alone; AVER_REPO_ROOT because the last section reads the
# HLSL files themselves.
add_executable(NeuRaCTest src/NeuRaCTest.cpp)
target_link_libraries(NeuRaCTest PRIVATE Aver.Core)
target_include_directories(NeuRaCTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(NeuRaCTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(NeuRaCTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# VoxiShiftTest -- the renderer's half of the floating origin that needs no device (LARGE_WORLD 2.2.6, section 5.4): the three previous-camera
# matrices against an independent f64 projection, NRD2's camera delta, NeuRaC's tag arithmetic for the three cell sizes, and source assertions that
# VoxiRenderer::shiftWorld, both backends and the water shaders touch every kept position. Links Aver.Core; the headers are reached by include path
# (NeuRaCLayout.hpp, rhi/WorldShift.hpp are Core-only); AVER_REPO_ROOT for the source assertions.
add_executable(VoxiShiftTest src/VoxiShiftTest.cpp)
target_link_libraries(VoxiShiftTest PRIVATE Aver.Core)
target_include_directories(VoxiShiftTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include ${CMAKE_SOURCE_DIR}/modules/rhi/include)
target_compile_definitions(VoxiShiftTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(VoxiShiftTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NeuRaCNetTest -- the stage-2a trust network's CPU half (aver/voxi/NeuRaCNetReference.hpp, the spec for
# voxi_neurac_net.hlsl): the feature builder on hand-built cells, label, eligibility, hash thinning, the trust
# mapping, the identity init (trust exactly 1), the batch against the Mlp's safeBatchLimit, and a
# MlpReference-trained net separating leaky from clean synthetic cells. Links Aver.Core and the Mlp's CPU
# reference; no device. AVER_REPO_ROOT because it also reads the HLSL's RCN_* #defines and entry points.
if(TARGET Aver.Render.Neural)
  add_executable(NeuRaCNetTest src/NeuRaCNetTest.cpp)
  target_link_libraries(NeuRaCNetTest PRIVATE Aver.Render.Neural Aver.Core)
  target_include_directories(NeuRaCNetTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
  target_compile_definitions(NeuRaCNetTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(NeuRaCNetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# SceneLightTest -- scene lights' CPU half (aver/voxi/SceneLight.hpp): candela -> engine-unit conversion,
# packing into the 80-byte AverLightRec, and the rectangle's clipped polygonal form factor against closed
# forms (distant point limit, infinite plane = pi, horizon clipping = pi/2). Links Aver.Core and
# Aver.Platform (to read the HLSL for the final layout check) and reaches the header by include path, as
# CameraFactorTest does.
add_executable(SceneLightTest src/SceneLightTest.cpp)
target_link_libraries(SceneLightTest PRIVATE Aver.Core Aver.Platform)
target_include_directories(SceneLightTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(SceneLightTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(SceneLightTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LightTreeTest -- many-light direct lighting's CPU half (aver/voxi/LightTree.hpp, docs/plans/wave4/MANY_LIGHTS.md section 5A):
# the BVH's structure and packing, canonical order and fill(), the importance bound against the light list's cheap
# irradiance, the descent's pdf against the path walk, a CPU twin of the RIS estimator against the brute-force sum (with
# the exact light excluded, the three-try redraw and roulette), and degenerate inputs. Compiles LightTree.cpp directly
# (Aver.Core only, no renderer), so the structure is tested without a device. AVER_REPO_ROOT: the last section reads
# voxi_lighttree.hlsli and checks its record offsets and flag bits against the header.
add_executable(LightTreeTest src/LightTreeTest.cpp ${CMAKE_SOURCE_DIR}/modules/render.voxi/src/LightTree.cpp)
target_link_libraries(LightTreeTest PRIVATE Aver.Core Aver.Platform)
target_include_directories(LightTreeTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(LightTreeTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(LightTreeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# RenderSettingsResolverTest -- the quality ladder (QualityLadder.hpp), the prerequisite resolver that
# decides what greys and why (RenderSettingsResolver.hpp), and the UE-style Overall Quality scalability
# preset (Scalability.hpp).
#
# Guarded by if(TARGET Aver.Render.Voxi): unlike CameraFactorTest and VoxiRtSeqTest above, this test
# DOES call exported Renderer functions (Renderer::get(), Renderer::setDeviceInfo(), the
# Renderer::*ForQuality statics) to exercise the one behaviour (giMode/denoiser stored as requested,
# resolved at read time) that only the live singleton's own setSettings() can regression-test -- so it
# needs the real DLL target to exist, not merely the header tree CameraFactorTest borrows.
if(TARGET Aver.Render.Voxi)
  add_executable(RenderSettingsResolverTest src/RenderSettingsResolverTest.cpp)
  target_link_libraries(RenderSettingsResolverTest PRIVATE Aver.Render.Voxi Aver.Core)
  set_target_properties(RenderSettingsResolverTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# VoxiShaderCompileTest -- compiles every staged ray-driven Voxi pipeline and every NRD2 pass on the
# adapter (no window, no scene), so shader errors fail here instead of in the editor. Skips (77)
# without ray tracing 1.1 / SM 6.6.
if(TARGET Aver.Render.Voxi.Renderer AND AVER_RHI_D3D12)
  add_executable(VoxiShaderCompileTest src/VoxiShaderCompileTest.cpp)
  target_link_libraries(VoxiShaderCompileTest PRIVATE Aver.Render.Voxi.Renderer Aver.Render.Voxi Aver.Render.Denoise Aver.Render.PBR Aver.RHI
                        Aver.RHI.D3D12 Aver.Core Aver.Platform)
  if(AVER_RHI_D3D11)
    target_link_libraries(VoxiShaderCompileTest PRIVATE Aver.RHI.D3D11)
  endif()
  if(AVER_RHI_VULKAN)
    target_link_libraries(VoxiShaderCompileTest PRIVATE Aver.RHI.Vulkan)
  endif()
  set_target_properties(VoxiShaderCompileTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# NeuRaCGpuTest -- the radiance cache's real scatter, lookup and resolve shaders against NeuRaCLayout.hpp's
# CPU mirrors, on a real device (WARP by default, `hw` for the adapter). Drives the real NeuRaC class and a
# small harness shader that includes voxi_neurac_io.hlsli; reads the shaders from the source tree through
# AVER_REPO_ROOT (setShaderSourceDir), so a stale bin/shaders deploy cannot hide an edit. Skips (77) without a
# D3D12 device, DXC / shader model 6.2, or IDevice::runStandaloneCompute. Links like VoxiShaderCompileTest.
if(TARGET Aver.Render.Voxi.Renderer AND AVER_RHI_D3D12)
  add_executable(NeuRaCGpuTest src/NeuRaCGpuTest.cpp)
  target_link_libraries(NeuRaCGpuTest PRIVATE Aver.Render.Voxi.Renderer Aver.Render.Voxi Aver.Render.Denoise Aver.Render.PBR Aver.RHI
                        Aver.RHI.D3D12 Aver.Core Aver.Platform)
  if(AVER_RHI_D3D11)
    target_link_libraries(NeuRaCGpuTest PRIVATE Aver.RHI.D3D11)
  endif()
  if(AVER_RHI_VULKAN)
    target_link_libraries(NeuRaCGpuTest PRIVATE Aver.RHI.Vulkan)
  endif()
  target_compile_definitions(NeuRaCGpuTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(NeuRaCGpuTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# VoxiSpirvCompileTest -- the Vulkan twin of VoxiShaderCompileTest, with no device: compiles Voxi's
# RayQuery scene/ray-driven variants, the water displacement kernel and the reference path tracer to
# SPIR-V through the vendored third_party/dxc-spirv DXC, with the argument list VulkanShaderCompiler
# uses. Links the generic RHI (shaderFile, the shared prelude) and the material prelude; no backend.
# Skips (77) without the vendored dxcompiler.dll. AVER_REPO_ROOT: stages the module shader trees.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(VoxiSpirvCompileTest src/VoxiSpirvCompileTest.cpp)
  # Embedded HLSL shaders are registered so the scene, ray-driven and compute variants are compiled WITH one (EmbeddedHlslBridge.hpp, hand-written sources:
  # the test needs no translator); the graph node lives in Aver.Formats.
  target_link_libraries(VoxiSpirvCompileTest PRIVATE Aver.Render.PBR.Materials Aver.Formats Aver.RHI Aver.Platform Aver.Core)
  target_include_directories(VoxiSpirvCompileTest PRIVATE ${CMAKE_SOURCE_DIR}/tests/render.pbr/src)
  target_compile_definitions(VoxiSpirvCompileTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(VoxiSpirvCompileTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# RasterLightsTest -- the raster local lights' CPU half (aver/voxi/RasterLights.hpp): culling, ranking and the 32-light cap, the parametric
# attenuation curves against their documented formulas, the energy calibration, the shadow-tile budget and atlas
# packing, the tile matrices, and the HLSL's constants read back from voxi.hlsl / voxi_locallights_raster.hlsli. Header-only, no device.
add_executable(RasterLightsTest src/RasterLightsTest.cpp)
target_link_libraries(RasterLightsTest PRIVATE Aver.Core Aver.Platform)
target_include_directories(RasterLightsTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include)
target_compile_definitions(RasterLightsTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(RasterLightsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CameraProjectionTest -- G4 R4: orthographic / frustum / keep_aspect projections (aver/core/CameraProjection.hpp) against reference values and
# unprojection round trips, render-layer masks and visibility ranges (aver/scene/RenderMasks.hpp), the component fields that carry them, and the
# raster light list's mask packing. Header-only, no device.
add_executable(CameraProjectionTest src/CameraProjectionTest.cpp)
target_link_libraries(CameraProjectionTest PRIVATE Aver.Core Aver.Platform)
target_include_directories(CameraProjectionTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/include ${CMAKE_SOURCE_DIR}/modules/formats/include ${CMAKE_SOURCE_DIR}/modules/scene/include)
target_compile_definitions(CameraProjectionTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(CameraProjectionTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
