# Accessibility (docs/ACCESSIBILITY.md): the option table, the caption engine, the speech queue, camera feel, the input
# assist and the C ABI over all of it. Everything but the ABI test is header-only logic compiled straight into the test.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.

# The key table, clamp/snap rules, INI reader and effective-options reduction. Header-only.
add_executable(AccessKeysTest src/AccessKeysTest.cpp)
target_link_libraries(AccessKeysTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(AccessKeysTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Cue matching, filtering, timing, line cap, speaker/colour/direction/fade. Header-only, no clock, no audio.
add_executable(CaptionEngineTest src/CaptionEngineTest.cpp)
target_link_libraries(CaptionEngineTest PRIVATE Aver.Framework Aver.Formats Aver.Core)
set_target_properties(CaptionEngineTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The speech queue over a fake synthesiser.
add_executable(SpeechQueueTest src/SpeechQueueTest.cpp)
target_link_libraries(SpeechQueueTest PRIVATE Aver.Framework Aver.Platform Aver.Core)
set_target_properties(SpeechQueueTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Shake and bob. Header-only.
add_executable(CameraFxTest src/CameraFxTest.cpp)
target_link_libraries(CameraFxTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(CameraFxTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The toggle latch and the trigger Timing. ActionEngine.cpp is internal to Aver.Framework (not exported), so the test
# compiles it in beside AccessAssist.hpp; Aver.Formats carries the .ocinput specs it parses.
add_executable(AccessAssistTest
  src/AccessAssistTest.cpp
  ${CMAKE_SOURCE_DIR}/modules/framework/src/ActionEngine.cpp)
target_include_directories(AccessAssistTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/framework/src)
target_link_libraries(AccessAssistTest PRIVATE Aver.Framework Aver.Formats Aver.Core)
set_target_properties(AccessAssistTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The C ABI end to end through the Aver.Framework DLL: options, captions from a folder, the audio observer path, the bus
# events, speech with whatever synthesiser the platform has (volume 0), camera feel.
add_executable(FrameworkAccessAbiTest src/FrameworkAccessAbiTest.cpp)
target_link_libraries(FrameworkAccessAbiTest PRIVATE Aver.Framework Aver.Formats Aver.Platform Aver.Core)
set_target_properties(FrameworkAccessAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
