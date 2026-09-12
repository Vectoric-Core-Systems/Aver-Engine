# NrdLinkTest -- NVIDIA NRD builds, links, and returns a coherent denoising plan. No GPU.
#
# Links Aver.Render.NRD and nothing else that matters: NRD is a planner, so everything it decides on
# the CPU is decidable here (see the file's own top comment). The suite stays GREEN in a build with
# AVER_WITH_NRD off -- it reports SKIP and returns 0 -- which is the point, because that build is a
# supported configuration, not a broken one.
add_executable(NrdLinkTest src/NrdLinkTest.cpp)
target_link_libraries(NrdLinkTest PRIVATE Aver.Render.NRD Aver.Core)
set_target_properties(NrdLinkTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NrdRecordTest -- the dispatches NrdLinkTest only PLANS, actually recorded onto a command list.
#
# The two are complementary and neither subsumes the other: NrdLinkTest needs no device and so runs
# everywhere, proving the library is linked and the plan is coherent; this one needs D3D12 (WARP is
# enough) and proves that plan can be turned into work. For most of this module's life the second
# question had never been asked, and the answer was no.
#
# D3D12 ONLY, DELIBERATELY. NRD wants its constant buffer and samplers in register space 1, which
# VulkanResourceFactory::descriptorLayout refuses on purpose -- so a Vulkan run of this test would
# report a documented design decision as a failure. It SKIPs rather than failing when D3D12 is
# absent, and when NRD is compiled out.
if(AVER_RHI_D3D12)
  add_executable(NrdRecordTest src/NrdRecordTest.cpp)
  target_link_libraries(NrdRecordTest PRIVATE Aver.Render.NRD Aver.RHI Aver.RHI.D3D12 Aver.Core Aver.Platform)
  # Its own composition root, exactly as every other device-creating test in this tree: Aver.RHI
  # dispatches to detail::create*Device behind AVER_HAS_*, so a consumer must supply every ENABLED
  # backend even when it only ever asks for one. Leaving these out links fine right up to LNK2019
  # on symbols this test never names.
  if(AVER_RHI_D3D11)
    target_link_libraries(NrdRecordTest PRIVATE Aver.RHI.D3D11)
  endif()
  if(AVER_RHI_VULKAN)
    target_link_libraries(NrdRecordTest PRIVATE Aver.RHI.Vulkan)
  endif()
  set_target_properties(NrdRecordTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# NrdNormalRoughnessEncodingTest -- the G-buffer's normal/roughness packing round-trips through
# NRD's own decoder, within R10G10B10A2's precision. See the file's own top comment for why this is
# CPU-only and why it carries a negative control.
#
# UNGUARDED, UNLIKE NrdRecordTest: averPackNormalRoughness and NRD's decode maths are both
# transcribed C++ here, not the real HLSL or the real library, so this needs neither AVER_WITH_NRD
# nor a D3D12 device -- it runs the same on every machine and every configuration.
add_executable(NrdNormalRoughnessEncodingTest src/NrdNormalRoughnessEncodingTest.cpp)
target_link_libraries(NrdNormalRoughnessEncodingTest PRIVATE Aver.Core)
set_target_properties(NrdNormalRoughnessEncodingTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
