# OcclusionMathTest -- the HZB conservative-culling math, with no GPU.
#
# Links Aver.Core alone: OcclusionMath.hpp (modules/occlusion/include/aver/occlusion/OcclusionMath.hpp)
# is header-only and RHI-free by design, so a box that should have stayed visible and did not is a
# defect this test can catch by arithmetic, before anything renders a single frame to compare against.
add_executable(OcclusionMathTest src/OcclusionMathTest.cpp)
target_link_libraries(OcclusionMathTest PRIVATE Aver.Occlusion Aver.Core)
set_target_properties(OcclusionMathTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
