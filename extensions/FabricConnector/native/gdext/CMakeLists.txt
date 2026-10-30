# Aver.Fabric.GDExt -- the GDExtension host of the Fabric Connector (docs/plans/fabric/G3_GDEXT.md).
#
# It loads a converted project's .gdextension plugins through aver::loadLibrary and serves the interface functions godot-cpp
# based extensions ask for (Variant, String, ClassDB registration, method binds) against a small engine class registry
# (Object, RefCounted, Resource, Node, Node3D). Two uses of the same sources:
#   Aver.Fabric.GDExt   static; the module, linked by the headless GdextHostTest
#   Aver.Fabric.Native  SHARED (../CMakeLists.txt); the sources are compiled into it, and its aver_gdext_* C ABI (fabric_gdext_abi.h) is what
#                       the Aver.Fabric C# runtime P/Invokes
# The interface header and API dump are Godot's, vendored in third_party/godot-gdextension (MIT). Nothing else is taken from godot-cpp.
set(AVER_GDEXT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextValue.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextOps.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextBuiltins.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextHost.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextEngine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextInterface.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextClassDb.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextWire.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/GdextAbi.cpp)

set(AVER_GDEXT_API_DIR ${AVER_FABRIC_DIR}/third_party/godot-gdextension)

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

set(AVER_GDEXT_SOURCES ${AVER_GDEXT_SOURCES} PARENT_SCOPE)
set(AVER_GDEXT_API_DIR ${AVER_GDEXT_API_DIR} PARENT_SCOPE)
set(AVER_GDEXT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include PARENT_SCOPE)
set(AVER_GDEXT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src PARENT_SCOPE)
