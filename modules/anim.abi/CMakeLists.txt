# Aver.Anim.Abi -- the plain-C seam onto property-track animation, so managed layers (an extension's animation player) drive it.
#
# SHARED because the .NET host P/Invokes it. It links Aver.Anim, Aver.Formats and Aver.Core STATICALLY and nothing else: no scene,
# no RHI, no window, so a managed test host loads it from a plain folder. The file is Aver.Anim.Abi and no managed assembly
# shares the name, so no NativeResolver tricks are needed beyond the shared one that finds Aver.* beside the executable.
add_library(Aver.Anim.Abi SHARED src/PropAnimAbi.cpp)

target_include_directories(Aver.Anim.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Anim.Abi PUBLIC cxx_std_20)
target_link_libraries(Aver.Anim.Abi PRIVATE Aver.Anim Aver.Formats Aver.Platform Aver.Core)
target_compile_definitions(Aver.Anim.Abi PRIVATE AVER_PANIM_ABI_BUILD)

set_target_properties(Aver.Anim.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # beside the exe, where DllImport looks
