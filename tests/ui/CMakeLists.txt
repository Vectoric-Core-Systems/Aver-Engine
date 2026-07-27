# UiTest — the draw list, with no GPU. That is the whole reason Aver.UI is Core-only: batching, clip
# nesting, layer separation and premultiplication are each a thing that looks fine on screen while
# being subtly wrong, and every one of them is decidable on the CPU.
add_executable(UiTest src/UiTest.cpp)
target_link_libraries(UiTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
