# WatcherTest -- DirectoryWatcher against a real filesystem.
#
# Real, not mocked, and the reason is in the test's own header: every hard case here is the operating
# system's behaviour rather than the class's, so a fake backend would be a test of the fake. It links
# Aver.Platform and Aver.Core and nothing else, which is the module's own dependency claim made into
# something the build enforces.
add_executable(WatcherTest src/WatcherTest.cpp)
target_link_libraries(WatcherTest PRIVATE Aver.Platform Aver.Core)
set_target_properties(WatcherTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# InputStateTest -- the opposite case to WatcherTest above. Nothing here is the operating system's
# behaviour: it is all edge detection, auto-repeat suppression and focus-loss clearing, which are
# the accumulator's own logic. So it is driven by synthetic events with no window at all, and runs
# in milliseconds with no scheduling noise.
add_executable(InputStateTest src/InputStateTest.cpp)
target_link_libraries(InputStateTest PRIVATE Aver.Platform Aver.Core)
set_target_properties(InputStateTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FileRangeTest -- aver::File, the ranged non-truncating handle the region format rests on.
#
# Real files, like WatcherTest above and for the same reason: whether a write at 4 GiB lands where it
# was asked is the operating system's behaviour, and a fake backend would return whatever it was told
# to and prove nothing. It uses a sparse file for the 64-bit offset case so the check costs no disk,
# and SKIPS that one case rather than lying if the filesystem declines.
add_executable(FileRangeTest src/FileRangeTest.cpp)
target_link_libraries(FileRangeTest PRIVATE Aver.Platform Aver.Core)
set_target_properties(FileRangeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# aver/core/DynamicLibrary.hpp lives in Aver.Core, but what it is checked against is the operating
# system's own loader, which is this directory's subject.
add_executable(DynamicLibraryTest src/DynamicLibraryTest.cpp)
target_link_libraries(DynamicLibraryTest PRIVATE Aver.Core)
set_target_properties(DynamicLibraryTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The SDL scancode -> Win32 virtual-key table, everywhere SDL is the window layer. A pure function, so
# it is checked with no display and no SDL_Init; it only needs SDL's headers to name the scancodes.
if(NOT WIN32)
  add_executable(SdlKeyMapTest src/SdlKeyMapTest.cpp)
  target_include_directories(SdlKeyMapTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/platform/src)
  target_link_libraries(SdlKeyMapTest PRIVATE Aver.Platform Aver.Core ${AVER_SDL3_TARGET})
  set_target_properties(SdlKeyMapTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ImageDecodeCorpus -- decodes every .webp / .svg / .exr under a directory it is given and prints counts (the
# owner's Godot corpus, kept outside the repo). With no argument it does nothing, so the ctest walk passes it.
add_executable(ImageDecodeCorpus src/ImageDecodeCorpus.cpp)
target_link_libraries(ImageDecodeCorpus PRIVATE Aver.Platform Aver.Core)
set_target_properties(ImageDecodeCorpus PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SpeechTest -- createSpeechSynth() never null and every call safe; on Windows a real SAPI voice at volume 0.
add_executable(SpeechTest src/SpeechTest.cpp)
target_link_libraries(SpeechTest PRIVATE Aver.Platform Aver.Core)
set_target_properties(SpeechTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# aver/core/CharConv.hpp: the floating std::from_chars fallback the macOS build depends on, checked on
# every platform -- including against std::from_chars wherever the standard library has it.
add_executable(CharConvTest src/CharConvTest.cpp)
target_link_libraries(CharConvTest PRIVATE Aver.Core)
set_target_properties(CharConvTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
