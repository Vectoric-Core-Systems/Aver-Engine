# Aver.Hair's tests. Every target name ends in Test, so the root CMakeLists' recursive registration (aver_register_ctest_in) picks them up with the
# FAIL_REGULAR_EXPRESSION every suite here is written against; a hand-written add_test would silently opt out of it.
#
# HairGenerateTest, HairAuthoringTest and HairLodTest are the pure half (the generator, glTF curves to strands, proxy hulls, the distance rules): Aver.Hair, Aver.Formats and Core, no
# physics at all -- the module's claim made into something the build enforces. HairSettingsBuildTest drives the physics ABI and so needs a world and
# the Aver.Physics DLL; it runs the groom on Jolt's CPU compute backend.
foreach(_t HairGenerateTest HairAuthoringTest HairLodTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Hair Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

add_executable(HairSettingsBuildTest src/HairSettingsBuildTest.cpp)
target_link_libraries(HairSettingsBuildTest PRIVATE Aver.Hair Aver.Physics Aver.Formats Aver.Core Aver.Platform)
set_target_properties(HairSettingsBuildTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# HairGroomEditorTest compiles the editor's Groom tab translation unit (sandbox/src/HairGroomTab.cpp) straight into the test, without ImGui, so only
# its model half exists (load, save, undo, every edit, the preview); with HairDetailsModel.hpp's rules and HairDebugOverlay.hpp's feeds that is the
# whole editor surface that does not need a window. The headers it includes are the sandbox's own, header-only (AssetEditor, SnapshotUndo).
add_executable(HairGroomEditorTest src/HairGroomEditorTest.cpp ${CMAKE_SOURCE_DIR}/sandbox/src/HairGroomTab.cpp)
target_include_directories(HairGroomEditorTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(HairGroomEditorTest PRIVATE Aver.Hair Aver.Physics Aver.Formats Aver.RHI Aver.Core Aver.Platform)
set_target_properties(HairGroomEditorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
