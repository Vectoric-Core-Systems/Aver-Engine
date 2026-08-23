# VoxiRtSeqTest -- the sun-disc sample sequence, with no GPU.
#
# It links Aver.Core and nothing else. The property under test is arithmetic -- that a sample's
# position does not depend on how many samples are being taken -- and arithmetic is exactly what a
# rendered image cannot decide: a shadow sampled by a completely different set of rays still looks
# like a shadow. The test reaches into the module's src/ for VoxiShaders.hpp, which is where the
# HLSL it asserts against lives.
add_executable(VoxiRtSeqTest src/VoxiRtSeqTest.cpp)
target_link_libraries(VoxiRtSeqTest PRIVATE Aver.Core)
target_include_directories(VoxiRtSeqTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/render.voxi/src)
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
