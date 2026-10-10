# Aver.Prefab -- prefab instances in a live scene::World: spawn, overrides, revert, apply, and the
# propagation of a prefab edit to every instance. See README.md and docs/PREFABS.md.
#
# A THIRD TARGET beside the world and the file format, for the reason Aver.Save is: Aver.Formats sits
# below Aver.Scene and may not reach it, Aver.Scene may not gain a Formats edge, so the join lives
# here and only a composition root links it.
#
# It does NOT link the renderer, physics or the editor. The editor's per-entity bookkeeping (bodies,
# outliner labels, level entity lists) follows through Hooks the host installs.
aver_add_module(Aver.Prefab
  SOURCES
    src/PrefabLibrary.cpp
    src/PrefabFields.cpp
    src/PrefabSystem.cpp
    src/PrefabEdit.cpp
    src/PrefabAbiHost.cpp
  DEPS
    Aver.Formats
    Aver.Scene
    Aver.Core
)

# LevelTransform.hpp is the .ocworld rotation contract: header-only and Core-only, so it is reached by
# include directory rather than by linking Aver.World (which would drag the renderer's noise code in).
target_include_directories(Aver.Prefab PRIVATE ${CMAKE_SOURCE_DIR}/modules/world/include)

# Aver.Prefab.Abi -- the plain-C seam a script spawns prefabs through. SHARED because the .NET host
# P/Invokes it, and STATELESS: the host installs function pointers (aver_prefab_set_host), so the one
# PrefabSystem lives in the editor or the runtime and this DLL never owns a world.
add_library(Aver.Prefab.Abi SHARED abi/PrefabAbi.cpp)
target_include_directories(Aver.Prefab.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Prefab.Abi PUBLIC cxx_std_20)
target_compile_definitions(Aver.Prefab.Abi PRIVATE AVER_PREFAB_ABI_BUILD)
# Core is static, so this DLL gets its own aver_prefab_last_error slot.
target_link_libraries(Aver.Prefab.Abi PRIVATE Aver.Core)
set_target_properties(Aver.Prefab.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # next to the exe, where DllImport looks
