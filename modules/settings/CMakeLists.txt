# Aver.Settings — durable key/value a shipped game can read and write.
#
# SHARED, like Aver.Scene, Aver.Framework and Aver.Physics, and for the identical reason those three
# are: the C# layer P/Invokes straight into it, and one binary means one store. There is no relay
# through the framework here because there is no link edge to avoid — settings depend on nothing
# above Core and Platform.
add_library(Aver.Settings SHARED src/Settings.cpp)

target_include_directories(Aver.Settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Settings PUBLIC cxx_std_20)
target_link_libraries(Aver.Settings PUBLIC Aver.Core Aver.Platform)

target_compile_definitions(Aver.Settings PRIVATE AVER_SETTINGS_BUILD)
target_compile_definitions(Aver.Settings PUBLIC AVER_MODULE_SETTINGS=1)

set_target_properties(Aver.Settings PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
