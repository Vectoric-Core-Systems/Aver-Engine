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
