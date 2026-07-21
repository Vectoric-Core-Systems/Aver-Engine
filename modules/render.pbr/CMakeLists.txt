# Aver.Render.PBR — the material system (see README.md).
#
# Built SHARED for the same reason Voxi is: the C# scripting layer P/Invokes this DLL, and keeping
# the material state in one binary means the editor and the bindings address the same materials.
# Depends only on Aver.Core, so nothing of the RHI leaks into the DLL — the moment `aver/rhi/*`
# appears behind this target, the scripting boundary is broken.
#
# aver_add_module() is STATIC-only, so a plain add_library is used here.
add_library(Aver.Render.PBR SHARED
  src/Material.cpp)

target_include_directories(Aver.Render.PBR PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.PBR PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.PBR PUBLIC Aver.Core)

# AVER_PBR_BUILD flips the export macros to dllexport while building this target.
target_compile_definitions(Aver.Render.PBR PRIVATE AVER_PBR_BUILD)
# Consumers get AVER_MODULE_PBR so they can #if around the whole feature.
target_compile_definitions(Aver.Render.PBR PUBLIC AVER_MODULE_PBR=1)

set_target_properties(Aver.Render.PBR PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe

# Aver.Render.PBR.Materials — the GPU half, deliberately a SECOND target.
# Static, because it is linked straight into the host executable, and separate because it needs the
# RHI: giving the DLL above that dependency would put render-hardware types on the P/Invoke boundary.
# Aver.RHI is the GENERIC interface. NEVER Aver.RHI.D3D12: no backend type may cross this boundary,
# and a link line is the only place that rule can actually be enforced.
add_library(Aver.Render.PBR.Materials STATIC
  src/MaterialGpu.cpp)

target_include_directories(Aver.Render.PBR.Materials PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.PBR.Materials PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.PBR.Materials PUBLIC Aver.Core Aver.RHI Aver.Render.PBR)

set_target_properties(Aver.Render.PBR.Materials PROPERTIES FOLDER "modules")
