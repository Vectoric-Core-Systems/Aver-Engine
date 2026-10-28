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
