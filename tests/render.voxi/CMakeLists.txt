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
