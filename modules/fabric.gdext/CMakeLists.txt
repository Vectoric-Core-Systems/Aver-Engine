# Aver.Fabric.GDExt -- the GDExtension host of the Fabric Connector (docs/plans/fabric/G3_GDEXT.md).
#
# It loads a converted project's .gdextension plugins through aver::loadLibrary and serves the interface functions godot-cpp
# based extensions ask for (Variant, String, ClassDB registration, method binds) against a small engine class registry
# (Object, RefCounted, Resource, Node, Node3D). Two targets over the same sources:
#   Aver.Fabric.GDExt      static; the module, linked by the headless test
#   Aver.Fabric.GDExt.Abi  SHARED; the aver_gdext_* C ABI (fabric_gdext_abi.h) the Aver.Fabric C# runtime P/Invokes
# The interface header and API dump are Godot's, vendored in third_party/godot-gdextension (MIT). Nothing else is taken from godot-cpp.
set(AVER_GDEXT_SOURCES
  src/GdextValue.cpp
  src/GdextOps.cpp
  src/GdextBuiltins.cpp
  src/GdextHost.cpp
  src/GdextEngine.cpp
  src/GdextInterface.cpp
  src/GdextClassDb.cpp
  src/GdextWire.cpp
  src/GdextAbi.cpp)

set(AVER_GDEXT_API_DIR ${CMAKE_SOURCE_DIR}/third_party/godot-gdextension)

aver_add_module(Aver.Fabric.GDExt
  SOURCES ${AVER_GDEXT_SOURCES}
  DEPS
    Aver.Formats.Godot
    Aver.Formats
    Aver.Platform
    Aver.Core
  PUBLIC_DEFS AVER_GDEXT_STATIC
)
target_include_directories(Aver.Fabric.GDExt PUBLIC ${AVER_GDEXT_API_DIR} PRIVATE src)

# Core is static, so this DLL has its own log sink and error slots, like the other *.Abi libraries.
add_library(Aver.Fabric.GDExt.Abi SHARED ${AVER_GDEXT_SOURCES})
target_include_directories(Aver.Fabric.GDExt.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE src ${AVER_GDEXT_API_DIR})
target_compile_features(Aver.Fabric.GDExt.Abi PUBLIC cxx_std_20)
target_compile_definitions(Aver.Fabric.GDExt.Abi PRIVATE AVER_GDEXT_ABI_BUILD)
target_link_libraries(Aver.Fabric.GDExt.Abi PRIVATE Aver.Formats.Godot Aver.Formats Aver.Platform Aver.Core)
set_target_properties(Aver.Fabric.GDExt.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # next to the exe, where DllImport looks

# The API dump the hash checks read: staged beside the executable, where the host looks for it by default.
add_custom_command(TARGET Aver.Fabric.GDExt.Abi POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AVER_GDEXT_API_DIR}/extension_api.json ${CMAKE_BINARY_DIR}/bin/extension_api.json
  COMMENT "Aver.Fabric.GDExt.Abi: extension_api.json -> bin")
