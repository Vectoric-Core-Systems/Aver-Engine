# Fabric Connector -- an engine extension that converts a Godot 4 project into an Aver project (docs/FABRIC.md,
# docs/plans/fabric/EXTENSION.md). The layout mirrors the Fabric-Connector repository it is to move to:
#
#   native/    the Godot readers (formats.godot), the GDExtension host (gdext) and the converter (tools/fabric), one SHARED library
#   managed/   Aver.Fabric (the runtime converted scripts compile against), Aver.Fabric.Convert (the script converter), Aver.Fabric.Tests
#   tests/     the C++ tests (tests/fabric, tests/formats.godot) and their fixtures
#   third_party/godot-gdextension  Godot's published GDExtension interface header and API dump (MIT)
#
# The engine reaches it only through the extension loader (modules/extensions): the output is bin/Extensions/FabricConnector/, which holds
# extension.json, native/<platform>/Aver.Fabric.Native.*, managed/ and data/. Nothing outside this directory links one of these targets.
set(AVER_FABRIC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(AVER_FABRIC_STAGE ${CMAKE_BINARY_DIR}/bin/Extensions/FabricConnector)

add_subdirectory(managed)   # before native: its targets are what the library's completeness depends on
add_subdirectory(native)
if(AVER_BUILD_TESTS)
  add_subdirectory(tests/formats.godot)
  add_subdirectory(tests/fabric)   # FabricConvertTest and the rest: AverAssetC fabric (through the loader) end to end
endif()
