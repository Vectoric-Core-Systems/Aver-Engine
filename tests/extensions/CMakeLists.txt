# ExtensionsTest -- the extension loader (modules/extensions) against fake extension directories and AverTestExt, a tiny extension written in C
# against aver_ext.h only (testext/aver_test_ext.c). AverTestExtRefuse is the same source whose entry refuses every host. Headless: no device,
# no .NET, no engine run. Registered with ctest by the root aver_register_ctest_in walk (it ends in Test).
add_library(AverTestExt SHARED testext/aver_test_ext.c)
target_include_directories(AverTestExt PRIVATE ${CMAKE_SOURCE_DIR}/modules/extensions/include)
set_target_properties(AverTestExt PROPERTIES
  FOLDER "tests"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ext_test
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ext_test
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ext_test_import)

add_library(AverTestExtRefuse SHARED testext/aver_test_ext.c)
target_include_directories(AverTestExtRefuse PRIVATE ${CMAKE_SOURCE_DIR}/modules/extensions/include)
target_compile_definitions(AverTestExtRefuse PRIVATE AVER_TEST_EXT_REFUSE)
set_target_properties(AverTestExtRefuse PROPERTIES
  FOLDER "tests"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ext_test
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ext_test
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ext_test_import)

# AverTestExt2 -- a second extension that uses a host service, offers an interface and reports through the command protocol (testext/aver_test_ext2.c).
add_library(AverTestExt2 SHARED testext/aver_test_ext2.c)
target_include_directories(AverTestExt2 PRIVATE ${CMAKE_SOURCE_DIR}/modules/extensions/include)
set_target_properties(AverTestExt2 PROPERTIES
  FOLDER "tests"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ext_test
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ext_test
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ext_test_import)

add_executable(ExtensionsTest src/ExtensionsTest.cpp)
target_link_libraries(ExtensionsTest PRIVATE Aver.Extensions Aver.Formats Aver.Platform Aver.Core)
add_dependencies(ExtensionsTest AverTestExt AverTestExtRefuse AverTestExt2)
target_compile_definitions(ExtensionsTest PRIVATE
  AVER_EXT_TEST_DLL="$<TARGET_FILE:AverTestExt>"
  AVER_EXT_TEST_REFUSE_DLL="$<TARGET_FILE:AverTestExtRefuse>"
  AVER_EXT_TEST2_DLL="$<TARGET_FILE:AverTestExt2>"
  AVER_EXT_TEST_WORKDIR="${CMAKE_BINARY_DIR}/Testing/extensions_work")
set_target_properties(ExtensionsTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AvextTest -- .avext packages (modules/extensions/src/Avext.cpp): digests, the zip reader on valid and hostile archives, the manifest read from a
# package before anything is copied, install (Replace, the trash, a refused package leaving nothing behind) and uninstall. Headless, no engine run.
# `AvextTest --avext <file> <exe dir>` installs a real package and prints what the loader says (the Fabric repository's check of its packed .avext).
add_executable(AvextTest src/AvextTest.cpp)
target_link_libraries(AvextTest PRIVATE Aver.Extensions Aver.Formats Aver.Platform Aver.Core)
target_compile_definitions(AvextTest PRIVATE AVER_AVEXT_TEST_WORKDIR="${CMAKE_BINARY_DIR}/Testing/avext_work")
set_target_properties(AvextTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ExtensionImportTest -- the headless half of the File menu's extension import dialog (sandbox/src/ExtensionImport.cpp, compiled without ImGui): the
# importers read through the loader from fake manifests, the input checks, the command line, the closing summary, and the importer running a fake
# tool (this executable) on a worker thread. No extension, no AverAssetC, no .NET, no engine run.
add_executable(ExtensionImportTest src/ExtensionImportTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/ExtensionImport.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/ProcessStream.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/EditorNotifications.cpp)
target_link_libraries(ExtensionImportTest PRIVATE Aver.Extensions Aver.Formats Aver.Platform Aver.Core)
target_include_directories(ExtensionImportTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
set_target_properties(ExtensionImportTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ManagedExtensionTest -- the managed half of an extension is loaded and its actor types declared through the manifest path, with the real CLR in the
# process (ScriptHost::init, HostDesc::extensionsDir) and no engine run. Needs the staged bridge and the sample assemblies (ActorScripts, SampleScripts),
# which Aver.Scripting.Host builds; it exits 77 (skipped) when they or .NET are absent.
if(TARGET Aver.Scripting.Host AND TARGET Aver.Framework)
  add_executable(ManagedExtensionTest src/ManagedExtensionTest.cpp)
  target_link_libraries(ManagedExtensionTest PRIVATE Aver.Extensions Aver.Framework Aver.Scripting.Host Aver.Formats Aver.Platform Aver.Core)
  add_dependencies(ManagedExtensionTest Aver.Scripting.Host)
  target_compile_definitions(ManagedExtensionTest PRIVATE AVER_MEXT_WORKDIR="${CMAKE_BINARY_DIR}/Testing/managed_extension_work")
  set_target_properties(ManagedExtensionTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # ManagedExtensionEntryTest -- the managed entries that cannot start (one that throws, one that does not exist): skipped with one log line. Its own
  # process, because two extensions cannot load the same assembly into one host context.
  add_executable(ManagedExtensionEntryTest src/ManagedExtensionEntryTest.cpp)
  target_link_libraries(ManagedExtensionEntryTest PRIVATE Aver.Extensions Aver.Framework Aver.Scripting.Host Aver.Formats Aver.Platform Aver.Core)
  add_dependencies(ManagedExtensionEntryTest Aver.Scripting.Host)
  target_compile_definitions(ManagedExtensionEntryTest PRIVATE AVER_MEXT_WORKDIR="${CMAKE_BINARY_DIR}/Testing/managed_extension_entry_work")
  set_target_properties(ManagedExtensionEntryTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
