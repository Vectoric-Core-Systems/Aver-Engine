# The native half of the Fabric Connector: Aver.Fabric.Native, one SHARED library per platform.
#
#   formats.godot  Aver.Formats.Godot   static; the Godot text/binary readers and the shading-language front end (Core + Platform only)
#   gdext          Aver.Fabric.GDExt    static; the GDExtension host, linked by the headless GdextHostTest
#   tools/fabric                        the converter (`AverAssetC fabric`), compiled into the library
#   src                                 aver_extension_entry: the extension ABI (modules/extensions/include/aver/extensions/aver_ext.h)
#
# The library exports two things. aver_extension_entry is what the engine's loader calls: runCommand "fabric" runs the converter. The
# aver_gdext_* functions (fabric_gdext_abi.h) are what Aver.Fabric's C# runtime P/Invokes to host a converted project's native plugins.
# It links its own copy of Core and the formats; only plain data crosses to the engine (aver_ext.h).
add_subdirectory(formats.godot)
add_subdirectory(gdext)

# The directory name the loader looks under: native/<platform>/ (Extensions.cpp platformDirName).
if(WIN32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(AVER_FABRIC_PLATFORM win-arm64)
  else()
    set(AVER_FABRIC_PLATFORM win-x64)
  endif()
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64|aarch64")
    set(AVER_FABRIC_PLATFORM osx-arm64)
  else()
    set(AVER_FABRIC_PLATFORM osx-x64)
  endif()
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64|aarch64")
    set(AVER_FABRIC_PLATFORM linux-arm64)
  else()
    set(AVER_FABRIC_PLATFORM linux-x64)
  endif()
endif()
set(AVER_FABRIC_PLATFORM ${AVER_FABRIC_PLATFORM} PARENT_SCOPE)

set(AVER_FABRIC_CONVERTER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/fabric)
set(AVER_FABRIC_CONVERTER_SOURCES
  ${AVER_FABRIC_CONVERTER_DIR}/FabricConvert.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricExtensions.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricProcess.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricScripts.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricMeshGen.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricModelTree.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricGeometry.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricAnimation.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricAnimTree.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricAudio.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricNav.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricNet.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricProject.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricReport.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricResources.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricShader.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricUi.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricWidgets.cpp
  ${AVER_FABRIC_CONVERTER_DIR}/FabricScene.cpp)
set(AVER_FABRIC_CONVERTER_SOURCES ${AVER_FABRIC_CONVERTER_SOURCES} PARENT_SCOPE)
set(AVER_FABRIC_CONVERTER_DIR ${AVER_FABRIC_CONVERTER_DIR} PARENT_SCOPE)

add_library(Aver.Fabric.Native SHARED
  ${AVER_GDEXT_SOURCES}
  ${AVER_FABRIC_CONVERTER_SOURCES}
  src/FabricExtension.cpp)
target_include_directories(Aver.Fabric.Native PRIVATE
  ${AVER_GDEXT_INCLUDE_DIR}        # fabric_gdext_abi.h
  ${AVER_GDEXT_SRC_DIR}            # the host's private headers
  ${AVER_GDEXT_API_DIR}            # gdextension_interface.h
  ${AVER_FABRIC_CONVERTER_DIR}
  ${CMAKE_SOURCE_DIR}/modules/world/include               # aver/world/LevelTransform.hpp (header-only)
  ${CMAKE_SOURCE_DIR}/modules/extensions/include)         # aver_ext.h and nothing else of the loader
target_compile_features(Aver.Fabric.Native PRIVATE cxx_std_20)
target_compile_definitions(Aver.Fabric.Native PRIVATE AVER_GDEXT_ABI_BUILD)
target_link_libraries(Aver.Fabric.Native PRIVATE Aver.Formats.Godot Aver.Formats Aver.UI Aver.Formats.Audio Aver.Audio Aver.Platform Aver.Core)
# The converter cooks the materials it converts when the material compiler exists (the same switch AverAssetC used).
if(TARGET Aver.Formats.Material)
  target_link_libraries(Aver.Fabric.Native PRIVATE Aver.Formats.Material)
  target_compile_definitions(Aver.Fabric.Native PRIVATE AVER_HAVE_MATERIAL_COMPILE=1)
  # ImportCook leaves the stb_image_write implementation to its executable; this library is one.
  target_sources(Aver.Fabric.Native PRIVATE src/StbImplementation.cpp)
  target_include_directories(Aver.Fabric.Native PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
endif()
# Aver.Fabric.Native.dll / .so / .dylib on every platform: the loader builds the name from the manifest (no "lib" prefix).
set_target_properties(Aver.Fabric.Native PROPERTIES
  FOLDER "extensions"
  PREFIX ""
  OUTPUT_NAME Aver.Fabric.Native
  RUNTIME_OUTPUT_DIRECTORY ${AVER_FABRIC_STAGE}/native/${AVER_FABRIC_PLATFORM}
  LIBRARY_OUTPUT_DIRECTORY ${AVER_FABRIC_STAGE}/native/${AVER_FABRIC_PLATFORM})

# Beside the library: the manifest and the API dump the method-hash checks read (data/extension_api.json). Real dependency edges, so an
# edit to either reaches bin/Extensions on the next build.
add_custom_command(
  OUTPUT ${AVER_FABRIC_STAGE}/extension.json
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AVER_FABRIC_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AVER_FABRIC_DIR}/extension.json ${AVER_FABRIC_STAGE}/extension.json
  DEPENDS ${AVER_FABRIC_DIR}/extension.json
  COMMENT "Fabric Connector: extension.json -> bin/Extensions/FabricConnector"
  VERBATIM)
add_custom_command(
  OUTPUT ${AVER_FABRIC_STAGE}/data/extension_api.json
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AVER_FABRIC_STAGE}/data
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AVER_GDEXT_API_DIR}/extension_api.json ${AVER_FABRIC_STAGE}/data/extension_api.json
  DEPENDS ${AVER_GDEXT_API_DIR}/extension_api.json
  COMMENT "Fabric Connector: extension_api.json -> bin/Extensions/FabricConnector/data"
  VERBATIM)
add_custom_target(Aver.Fabric.Stage DEPENDS ${AVER_FABRIC_STAGE}/extension.json ${AVER_FABRIC_STAGE}/data/extension_api.json)
set_target_properties(Aver.Fabric.Stage PROPERTIES FOLDER "extensions")
add_dependencies(Aver.Fabric.Native Aver.Fabric.Stage)

# The managed half (managed/Aver.Fabric.dll, which the manifest lists, and tools/Aver.Fabric.Convert.dll) is built by ../managed; the extension is only
# complete once both exist.
foreach(_aver_fabric_managed Aver.Fabric.Managed Aver.Fabric.Convert)
  if(TARGET ${_aver_fabric_managed})
    add_dependencies(Aver.Fabric.Native ${_aver_fabric_managed})
  endif()
endforeach()
