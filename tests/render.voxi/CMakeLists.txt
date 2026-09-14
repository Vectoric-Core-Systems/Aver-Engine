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

# CameraFactorTest -- recovering NRD's separate worldToView/viewToClip from the combined viewProj
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
