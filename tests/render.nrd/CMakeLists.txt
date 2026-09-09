# NrdLinkTest -- NVIDIA NRD builds, links, and returns a coherent denoising plan. No GPU.
#
# Links Aver.Render.NRD and nothing else that matters: NRD is a planner, so everything it decides on
# the CPU is decidable here (see the file's own top comment). The suite stays GREEN in a build with
# AVER_WITH_NRD off -- it reports SKIP and returns 0 -- which is the point, because that build is a
# supported configuration, not a broken one.
add_executable(NrdLinkTest src/NrdLinkTest.cpp)
target_link_libraries(NrdLinkTest PRIVATE Aver.Render.NRD Aver.Core)
set_target_properties(NrdLinkTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
