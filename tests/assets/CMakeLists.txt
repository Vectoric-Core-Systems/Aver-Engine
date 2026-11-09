# TextureStreamMathTest -- the mip-window arithmetic, the .octex header-only stat and ranged mip reads, and the
# streamable predicate. TexturePlannerTest -- the pure residency planner, pool controller and upload ring with
# synthetic footprints. Neither touches a device or a window.
add_executable(TextureStreamMathTest src/TextureStreamMathTest.cpp)
target_link_libraries(TextureStreamMathTest PRIVATE Aver.Assets.Gpu Aver.Assets Aver.Formats Aver.Platform Aver.Core)
set_target_properties(TextureStreamMathTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(TexturePlannerTest src/TexturePlannerTest.cpp)
target_link_libraries(TexturePlannerTest PRIVATE Aver.Assets Aver.Core)
set_target_properties(TexturePlannerTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
