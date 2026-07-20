# Aver.Render.Voxi — optional render-feature module (see README.md).
#
# Built SHARED on purpose: the C# scripting layer P/Invokes this DLL, and keeping the settings
# state in one binary means the editor and the bindings agree on the contract. Depends only on
# Aver.Core, so nothing of the RHI leaks into the DLL.
add_library(Aver.Render.Voxi SHARED
  src/Voxi.cpp)

target_include_directories(Aver.Render.Voxi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Voxi PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.Voxi PUBLIC Aver.Core)

# AVER_VOXI_BUILD flips the export macros to dllexport while building this target.
target_compile_definitions(Aver.Render.Voxi PRIVATE AVER_VOXI_BUILD)
# Consumers get AVER_MODULE_VOXI so they can #if around the whole feature.
target_compile_definitions(Aver.Render.Voxi PUBLIC AVER_MODULE_VOXI=1)

set_target_properties(Aver.Render.Voxi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe

# Aver.Render.Voxi.Renderer — the GPU half of the feature, deliberately a SECOND target.
# Static, because it is linked straight into the host executable, and separate because it needs the
# RHI: giving the DLL above that dependency would put render-hardware types on the P/Invoke boundary.
add_library(Aver.Render.Voxi.Renderer STATIC
  src/VoxiRenderer.cpp)

target_include_directories(Aver.Render.Voxi.Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Voxi.Renderer PUBLIC cxx_std_20)
# Aver.RHI is the GENERIC interface. Never Aver.RHI.D3D12: no backend type may cross this boundary,
# and a link line is the only place that rule can actually be enforced.
target_link_libraries(Aver.Render.Voxi.Renderer PUBLIC Aver.Core Aver.RHI Aver.Render.Voxi)

set_target_properties(Aver.Render.Voxi.Renderer PROPERTIES FOLDER "modules")
