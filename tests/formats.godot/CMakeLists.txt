# FormatsGodotTest -- the Godot text-format reader (modules/formats.godot) over the hand-written
# fixtures in tests/fabric/data/. Core and Platform only: no device, no .NET, no engine.
# Registered with ctest by the root aver_register_ctest_in walk (it ends in Test).
add_executable(FormatsGodotTest src/FormatsGodotTest.cpp)
target_link_libraries(FormatsGodotTest PRIVATE Aver.Formats.Godot Aver.Platform Aver.Core)
target_compile_definitions(FormatsGodotTest PRIVATE AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FormatsGodotTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GodotMeshTest -- G4 R7: ArrayMesh surfaces decoded from hand-built byte arrays, and GridMap's packed cells and 24 bases (pure data).
add_executable(GodotMeshTest src/GodotMeshTest.cpp)
target_link_libraries(GodotMeshTest PRIVATE Aver.Formats.Godot Aver.Platform Aver.Core)
set_target_properties(GodotMeshTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GodotShaderTest -- G6 SH1: the Godot shading-language front-end (preprocessor, parser + type checker, HLSL emitter). The HLSL it emits is compiled by the
# engine's DXC to DXIL (dxcompiler.dll beside the test) and to SPIR-V (third_party/dxc-spirv); no device, no window. Aver.RHI is linked for dxcapi.h only.
# The DXC halves skip (and say so) when a compiler DLL is missing.
if(TARGET Aver.RHI)
  add_executable(GodotShaderTest src/GodotShaderTest.cpp)
  target_link_libraries(GodotShaderTest PRIVATE Aver.Formats.Godot Aver.RHI Aver.Platform Aver.Core)
  target_compile_definitions(GodotShaderTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(GodotShaderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# GodotShaderMaterialTest -- G6 SH2: a Godot spatial shader on an Aver material. The engine's own translation unit (shared prelude + material prelude + the
# registry's generated code) is compiled with DXC to DXIL and SPIR-V, and the converter-side preparation (uniform table, sampler slots, render modes, the
# report) and the GodotShader graph node are checked. No device, no window.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(GodotShaderMaterialTest src/GodotShaderMaterialTest.cpp)
  target_link_libraries(GodotShaderMaterialTest PRIVATE Aver.Formats.Godot Aver.Render.PBR.Materials Aver.Formats Aver.RHI Aver.Platform Aver.Core)
  target_compile_definitions(GodotShaderMaterialTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(GodotShaderMaterialTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# GodotShaderEnvTest -- G6 SH3: Godot sky, fog and particles shaders. The front-end's preparation, the GodotEnvShader graph node, and the engine's real translation units
# (Voxi with the sky spliced into the shared prelude, the froxel fog passes with fog shaders, the GPU particle simulation with a particles shader) compiled with DXC to
# DXIL and SPIR-V. No device, no window.
if(TARGET Aver.Render.PBR.Materials)
  add_executable(GodotShaderEnvTest src/GodotShaderEnvTest.cpp)
  target_link_libraries(GodotShaderEnvTest PRIVATE Aver.Formats.Godot Aver.Render.PBR.Materials Aver.Formats Aver.RHI Aver.Platform Aver.Core)
  target_compile_definitions(GodotShaderEnvTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(GodotShaderEnvTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
