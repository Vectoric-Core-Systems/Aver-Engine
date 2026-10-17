# Model import C ABI (framework_import_abi.h): the relay to an installed provider and the refusal without one.
# Links the Aver.Framework DLL only: the provider is a fake here, so no importer is needed.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkImportAbiTest src/FrameworkImportAbiTest.cpp)
target_link_libraries(FrameworkImportAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkImportAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
