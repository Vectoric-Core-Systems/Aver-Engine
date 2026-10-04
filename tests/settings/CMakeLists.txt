# SettingsTest — the store a shipped game keeps its volume in.
add_executable(SettingsTest src/SettingsTest.cpp)
target_link_libraries(SettingsTest PRIVATE Aver.Settings Aver.Core Aver.Platform)
set_target_properties(SettingsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
