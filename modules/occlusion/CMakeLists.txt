# Aver.Occlusion -- hierarchical-Z occlusion culling (see README.md and include/aver/occlusion/
# Occlusion.hpp for the design). ONE target, not split into a CPU-model/renderer pair the way
# Aver.Landscape and Aver.Render.Voxi.Renderer are: OcclusionMath.hpp, the piece that actually needs
# to be checkable with no GPU, is header-only and pulls in nothing from this target at all -- a test
# can include it directly against Aver.Core alone (see tests/occlusion/CMakeLists.txt) without this
# library existing. What is left in OcclusionCuller.cpp is GPU-driving code through and through, so
# giving it its own RHI-free half would just be an empty target.
add_library(Aver.Occlusion STATIC
  src/OcclusionCuller.cpp)

target_include_directories(Aver.Occlusion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Occlusion PUBLIC cxx_std_20)

# Aver.RHI is the GENERIC interface. Never Aver.RHI.D3D12 or Aver.RHI.Vulkan: no backend type may
# cross this boundary, and a link line is the only place that rule can actually be enforced -- see
# modules/render.voxi/CMakeLists.txt's identical comment on Aver.Render.Voxi.Renderer.
target_link_libraries(Aver.Occlusion PUBLIC Aver.Core Aver.RHI)

# Consumers get AVER_MODULE_OCCLUSION so they can #if around the whole feature, same idiom as every
# other optional module (AVER_MODULE_VOXI, AVER_MODULE_LANDSCAPE, ...).
target_compile_definitions(Aver.Occlusion PUBLIC AVER_MODULE_OCCLUSION=1)

set_target_properties(Aver.Occlusion PROPERTIES FOLDER "modules")
