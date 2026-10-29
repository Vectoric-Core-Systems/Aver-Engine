# FabricConvertTest -- `AverAssetC fabric` over the hand-written Godot project in tests/fabric/data/ConvertGame, then every
# file it wrote read back with the engine's own loaders (.ocproject, .ocworld, .ocprefab, .ocinput, .ocmat, .ocmesh).
# It runs the tool as a process, like AverAssetCTest, so it depends on that target. No device, no .NET, no engine run.
# Registered with ctest by the root aver_register_ctest_in walk (it ends in Test).
add_executable(FabricConvertTest src/FabricConvertTest.cpp)
target_link_libraries(FabricConvertTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
if(TARGET Aver.Formats.Material)
  target_link_libraries(FabricConvertTest PRIVATE Aver.Formats.Material Aver.Render.PBR.Materials)
  target_compile_definitions(FabricConvertTest PRIVATE AVER_FABRIC_TEST_OCMAT=1)
endif()
add_dependencies(FabricConvertTest AverAssetC)
target_include_directories(FabricConvertTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/world/include)
target_compile_definitions(FabricConvertTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricConvertTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricMeshExtraTest -- G4 R1 (vertex colour, tangent, UV2) through the converter on tests/fabric/data/ExtraGame: the extra streams in the
# converted .ocmesh files and the vertexcolor flag in the .ocmat. Runs the tool as a process, like FabricConvertTest.
add_executable(FabricMeshExtraTest src/FabricMeshExtraTest.cpp)
target_link_libraries(FabricMeshExtraTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
if(TARGET Aver.Formats.Material)
  target_link_libraries(FabricMeshExtraTest PRIVATE Aver.Formats.Material Aver.Render.PBR.Materials)
  target_compile_definitions(FabricMeshExtraTest PRIVATE AVER_FABRIC_TEST_OCMAT=1)
endif()
add_dependencies(FabricMeshExtraTest AverAssetC)
target_compile_definitions(FabricMeshExtraTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricMeshExtraTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GdextHostTest -- the GDExtension host (modules/fabric.gdext) against FabricTestExt, a GDExtension written in C against
# gdextension_interface.h only (gdext_test/fabric_test_ext.c; no godot-cpp). The extension and the .gdextension fixtures
# (data/gdext/) land in bin/gdext_test. Headless: no device, no .NET, no engine run.
add_library(FabricTestExt SHARED gdext_test/fabric_test_ext.c)
target_include_directories(FabricTestExt PRIVATE ${CMAKE_SOURCE_DIR}/third_party/godot-gdextension)
set_target_properties(FabricTestExt PROPERTIES
  OUTPUT_NAME FabricTestExt
  FOLDER "tests"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/gdext_test
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/gdext_test
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gdext_test_import)
add_custom_command(TARGET FabricTestExt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data/gdext ${CMAKE_BINARY_DIR}/bin/gdext_test
  COMMENT "FabricTestExt: .gdextension fixtures -> bin/gdext_test")

add_executable(GdextHostTest src/GdextHostTest.cpp)
target_link_libraries(GdextHostTest PRIVATE Aver.Fabric.GDExt Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
target_include_directories(GdextHostTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/fabric.gdext/src)
add_dependencies(GdextHostTest FabricTestExt)
target_compile_definitions(GdextHostTest PRIVATE
  AVER_GDEXT_TEST_DIR="${CMAKE_BINARY_DIR}/bin/gdext_test"
  AVER_GDEXT_API_JSON="${CMAKE_SOURCE_DIR}/third_party/godot-gdextension/extension_api.json"
  AVER_GDEXT_HEADER="${CMAKE_SOURCE_DIR}/third_party/godot-gdextension/gdextension_interface.h")
set_target_properties(GdextHostTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricE2ETest -- the whole connector through its real command line on tests/fabric/data/E2EGame: scenes, assets, GDScript and GodotSharp
# conversion (Aver.Fabric.Convert on dotnet, compiled against Aver.Fabric), the GDExtension plugin (FabricTestExt, copied in under the
# three platform names) and the merged report. Needs the script converter and the managed runtime staged beside AverAssetC, so it
# depends on those targets when they exist and skips (77) when dotnet or they are absent.
add_executable(FabricE2ETest src/FabricE2ETest.cpp ${CMAKE_SOURCE_DIR}/tools/fabric/FabricProcess.cpp)
target_link_libraries(FabricE2ETest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
target_include_directories(FabricE2ETest PRIVATE ${CMAKE_SOURCE_DIR}/tools ${CMAKE_SOURCE_DIR}/modules/world/include)
add_dependencies(FabricE2ETest AverAssetC FabricTestExt)
if(TARGET Aver.Fabric.Convert)
  add_dependencies(FabricE2ETest Aver.Fabric.Convert)
endif()
if(TARGET Aver.Scripting.Bridge)
  add_dependencies(FabricE2ETest Aver.Scripting.Bridge)
endif()
target_compile_definitions(FabricE2ETest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_TEST_EXT="$<TARGET_FILE:FabricTestExt>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricE2ETest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricImportTest -- the headless half of File > Import Godot Project... (sandbox/src/FabricImport.cpp, compiled in without ImGui):
# folder checks, the command line, the converter's summary, and the importer running the real AverAssetC on a worker thread.
add_executable(FabricImportTest src/FabricImportTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/FabricImport.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/EditorNotifications.cpp
  ${CMAKE_SOURCE_DIR}/tools/fabric/FabricProcess.cpp)
target_link_libraries(FabricImportTest PRIVATE Aver.Platform Aver.Core)
target_include_directories(FabricImportTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src ${CMAKE_SOURCE_DIR}/tools)
add_dependencies(FabricImportTest AverAssetC)
if(TARGET Aver.Fabric.Convert)
  add_dependencies(FabricImportTest Aver.Fabric.Convert)
endif()
if(TARGET Aver.Scripting.Bridge)
  add_dependencies(FabricImportTest Aver.Scripting.Bridge)
endif()
target_compile_definitions(FabricImportTest PRIVATE AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricImportTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricLightsTest -- G4 R2 (omni/spot/sun calibration and ratings) through the converter on tests/fabric/data/LightsGame: the SUN record's lux,
# the light Props, one sun, and the per-property report. Runs the tool as a process, like FabricConvertTest.
add_executable(FabricLightsTest src/FabricLightsTest.cpp)
target_link_libraries(FabricLightsTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricLightsTest AverAssetC)
target_compile_definitions(FabricLightsTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricLightsTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricCameraTest -- G4 R4 (camera projection, keep_aspect, offsets, cull masks, render layers, visibility ranges) through the converter on
# tests/fabric/data/CameraGame: the Props, the made-current first camera, and the per-property report. Runs the tool as a process.
add_executable(FabricCameraTest src/FabricCameraTest.cpp)
target_link_libraries(FabricCameraTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricCameraTest AverAssetC)
target_compile_definitions(FabricCameraTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricCameraTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricEnvTest -- G4 R3 (WorldEnvironment, tonemapper, adjustments, fog, panorama sky, CameraAttributes) through the converter on
# tests/fabric/data/EnvGame, and LightsGame (no environment) for the unchanged default. Runs the tool as a process, like FabricConvertTest.
add_executable(FabricEnvTest src/FabricEnvTest.cpp)
target_link_libraries(FabricEnvTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricEnvTest AverAssetC)
target_compile_definitions(FabricEnvTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricEnvTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricEnvVolTest -- G4 R8 (FogVolume + FogMaterial, ReflectionProbe, LightmapGI, LightmapProbe, VoxelGI, volumetric fog, the screen-space / SDF flags) through the
# converter on tests/fabric/data/EnvVolGame; EnvGame and LightsGame for the unchanged default. Runs the tool as a process, like FabricEnvTest.
add_executable(FabricEnvVolTest src/FabricEnvVolTest.cpp)
target_link_libraries(FabricEnvVolTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricEnvVolTest AverAssetC)
target_compile_definitions(FabricEnvVolTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricEnvVolTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricViewportTest -- G4 R5 (SubViewport, ViewportTexture, SubViewportContainer) through the converter on tests/fabric/data/ViewportGame: the view's key and
# consumers, the '@rt:viewport.<key>' texture reference in the .ocmat, cameras below a view, the container's Props, and the per-property report. Runs the tool
# as a process, like FabricConvertTest.
add_executable(FabricViewportTest src/FabricViewportTest.cpp)
target_link_libraries(FabricViewportTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
if(TARGET Aver.Formats.Material)
  target_link_libraries(FabricViewportTest PRIVATE Aver.Formats.Material Aver.Render.PBR.Materials)
  target_compile_definitions(FabricViewportTest PRIVATE AVER_FABRIC_TEST_OCMAT=1)
endif()
add_dependencies(FabricViewportTest AverAssetC)
target_compile_definitions(FabricViewportTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricViewportTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricMaterialFxTest -- G4 R6 (StandardMaterial3D / ORMMaterial3D features: UV transforms, triplanar, cull / blend / depth modes, alpha hash, billboard,
# fades, rim, clearcoat, parallax, detail ...) through the converter on tests/fabric/data/MatGame: the .ocmat files read back with the engine's own loader
# and the per-property report. Runs the tool as a process, like FabricConvertTest.
if(TARGET Aver.Formats.Material)
  add_executable(FabricMaterialFxTest src/FabricMaterialFxTest.cpp)
  target_link_libraries(FabricMaterialFxTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Formats.Material Aver.Render.PBR.Materials Aver.Platform Aver.Core)
  add_dependencies(FabricMaterialFxTest AverAssetC)
  target_compile_definitions(FabricMaterialFxTest PRIVATE
    AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
    AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
  set_target_properties(FabricMaterialFxTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# FabricGeometryTest -- G4 R7 (ArrayMesh, MultiMesh, trail meshes, CSG, Sprite3D, Label3D, GridMap) through the converter on tests/fabric/data/GeomGame.
# Runs the tool as a process, like FabricConvertTest.
add_executable(FabricGeometryTest src/FabricGeometryTest.cpp)
target_link_libraries(FabricGeometryTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
if(TARGET Aver.Formats.Material)
  target_link_libraries(FabricGeometryTest PRIVATE Aver.Formats.Material Aver.Render.PBR.Materials)
  target_compile_definitions(FabricGeometryTest PRIVATE AVER_FABRIC_TEST_OCMAT=1)
endif()
add_dependencies(FabricGeometryTest AverAssetC)
target_compile_definitions(FabricGeometryTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricGeometryTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricAnimTest -- G5 A1 (AnimationPlayer) through the converter on tests/fabric/data/AnimGame: Animation sub-resources and an external .tres become
# .ocanim files with property tracks (transform tracks through the axis map), the player node keeps its libraries, and every track is rated.
add_executable(FabricAnimTest src/FabricAnimTest.cpp)
target_link_libraries(FabricAnimTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricAnimTest AverAssetC)
target_compile_definitions(FabricAnimTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricAnimTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricAudioTest -- G5 S1 (audio) through the converter on tests/fabric/data/AudioGame: sound files copied under Content/Audio, stream resources as Object(...) values, the audio
# nodes placed as their classes, the default AudioBusLayout as an .ocbus named by AUDIO.BUSLAYOUT, and every item rated (the Ogg / MP3 decoders, the effects, VideoStreamPlayer).
add_executable(FabricAudioTest src/FabricAudioTest.cpp)
target_link_libraries(FabricAudioTest PRIVATE Aver.Formats.Audio Aver.Formats.Godot Aver.Formats Aver.Audio Aver.Platform Aver.Core)
add_dependencies(FabricAudioTest AverAssetC)
target_compile_definitions(FabricAudioTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricAudioTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricNavTest -- G5 N1 (navigation) through the converter on tests/fabric/data/NavGame: regions, links, obstacles and agents placed as their classes, baked
# NavigationMesh resources as Object(...) values, a never-baked mesh flagged for a run-time bake, and every node and baking parameter rated.
add_executable(FabricNavTest src/FabricNavTest.cpp)
target_link_libraries(FabricNavTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricNavTest AverAssetC)
target_compile_definitions(FabricNavTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricNavTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricNetTest -- G6 NET2 (high-level multiplayer) through the converter on tests/fabric/data/NetGame: MultiplayerSpawner / MultiplayerSynchronizer placed as their classes, the
# SceneReplicationConfig sub-resource as an Object(...) value, the spawnable scene list as converted level paths, and every node, property and entry rated.
add_executable(FabricNetTest src/FabricNetTest.cpp)
target_link_libraries(FabricNetTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricNetTest AverAssetC)
target_compile_definitions(FabricNetTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricNetTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricUiTest -- G5 U1 (Control foundations) through the converter on tests/fabric/data/UiGame: the Control family placed as its classes, style boxes as Object(...) values,
# Theme resources as .octheme files the engine's parser reads, the project's stretch / theme / tooltip settings as UI.* keys, and every property rated.
add_executable(FabricUiTest src/FabricUiTest.cpp)
target_link_libraries(FabricUiTest PRIVATE Aver.UI Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricUiTest AverAssetC)
target_compile_definitions(FabricUiTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricUiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricWidgetTest -- G5 U2 (widgets and rich text) through the converter on tests/fabric/data/WidgetGame: widgets placed as their classes, every property rated, LabelSettings /
# FontVariation / SystemFont / ButtonGroup / Shortcut as Object(...) values or one shared Resource(), and UI/Fonts.ocfonts the engine's font set parser reads.
add_executable(FabricWidgetTest src/FabricWidgetTest.cpp)
target_link_libraries(FabricWidgetTest PRIVATE Aver.UI Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricWidgetTest AverAssetC)
target_compile_definitions(FabricWidgetTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricWidgetTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricTreeTest -- G5 A2 (AnimationTree, skeleton, path and helper nodes) through the converter on tests/fabric/data/TreeGame: the tree's graph becomes one
# Object(...) value, the nodes are placed as their classes, curves and bones keep their data, and every node and property is rated.
add_executable(FabricTreeTest src/FabricTreeTest.cpp)
target_link_libraries(FabricTreeTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricTreeTest AverAssetC)
target_compile_definitions(FabricTreeTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricTreeTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricCorpusPatternsTest -- the patterns two Godot demo projects showed, on tests/fabric/data/CorpusGame: children and overrides below the nodes of an
# instanced glTF model, Godot 3 key codes and joypad numbers, libraries/ properties, a curve's _limits. Runs the tool as a process, like FabricTreeTest.
add_executable(FabricCorpusPatternsTest src/FabricCorpusPatternsTest.cpp)
target_link_libraries(FabricCorpusPatternsTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricCorpusPatternsTest AverAssetC)
target_compile_definitions(FabricCorpusPatternsTest PRIVATE
  AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
  AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
set_target_properties(FabricCorpusPatternsTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FabricShaderTest -- G6 SH2 (ShaderMaterial over .gdshader: the shader written as a material graph, the material as GRAPHREF + FX + GDREGS + TEX slots) through the
# converter on tests/fabric/data/ShaderGame. Runs the tool as a process, like FabricMaterialFxTest.
if(TARGET Aver.Formats.Material)
  add_executable(FabricShaderTest src/FabricShaderTest.cpp)
  target_link_libraries(FabricShaderTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Formats.Material Aver.Render.PBR.Materials Aver.RHI Aver.Platform Aver.Core)
  add_dependencies(FabricShaderTest AverAssetC)
  target_compile_definitions(FabricShaderTest PRIVATE
    AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
    AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
  set_target_properties(FabricShaderTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# FabricEnvShaderTest -- G6 SH3 (sky, fog and particles shaders, GPUParticles3D, VisualShader) through the converter on tests/fabric/data/EnvShaderGame.
if(TARGET Aver.Formats.Material AND TARGET Aver.Formats.Particles)
  add_executable(FabricEnvShaderTest src/FabricEnvShaderTest.cpp)
  target_link_libraries(FabricEnvShaderTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Formats.Material Aver.Formats.Particles Aver.Render.PBR.Materials Aver.RHI Aver.Platform Aver.Core)
  add_dependencies(FabricEnvShaderTest AverAssetC)
  target_compile_definitions(FabricEnvShaderTest PRIVATE
    AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>"
    AVER_FABRIC_DATA="${CMAKE_SOURCE_DIR}/tests/fabric/data")
  set_target_properties(FabricEnvShaderTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# FabricBinaryTest -- binary .res / .scn (RSRC) through the converter: a binary ArrayMesh, BoxShape3D and GridMap scene built by the test (fixtures shared with
# tests/formats.godot/src/GodotBinaryTest.cpp), converted by the tool run as a process. No device, no .NET, no engine run.
add_executable(FabricBinaryTest src/FabricBinaryTest.cpp)
target_link_libraries(FabricBinaryTest PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
add_dependencies(FabricBinaryTest AverAssetC)
target_compile_definitions(FabricBinaryTest PRIVATE AVER_ASSETC_EXE="$<TARGET_FILE:AverAssetC>")
set_target_properties(FabricBinaryTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
