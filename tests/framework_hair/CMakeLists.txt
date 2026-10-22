# Strand hair C ABI (framework_hair_abi.h): the relay to an installed provider, the zeros without one, and the refusals that never reach the
# provider. The provider is a fake here, so no hair module is needed. The relay itself is compiled into Aver.Framework from
# modules/hair.scene/framework/FrameworkHairAbi.cpp (modules/framework/CMakeLists.txt), so this links the DLL only.
#
# HairProviderTest drives hair::hairProvider on a HairSystem with no world content: the validation and the answers that need no groom.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkHairAbiTest src/FrameworkHairAbiTest.cpp)
target_link_libraries(FrameworkHairAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkHairAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(TARGET Aver.Hair.Scene)
  add_executable(HairProviderTest src/HairProviderTest.cpp)
  target_link_libraries(HairProviderTest PRIVATE Aver.Hair.Scene Aver.Scene Aver.Physics Aver.Formats Aver.Core)
  set_target_properties(HairProviderTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
