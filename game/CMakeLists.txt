# AverGame.exe -- the standalone game runtime.
#
# WIN32 (not a console subsystem): a shipped game that opens a console window behind itself looks
# broken. EntryPoint.hpp generates a standard main(), so the linker is told where to find it rather
# than the source being changed.
#
# This comment used to say "for the same reason Sandbox is", which was never true -- Sandbox was
# console-subsystem until 0.5.0, and the console it opened beside the editor is what finally made
# someone check. It says the reason itself now, rather than pointing at a target that did not share it.
add_executable(AverGame WIN32 src/GameMain.cpp)
target_link_libraries(AverGame PRIVATE Aver.Runtime.Game Aver.Runtime Aver.Platform Aver.Core)

# AverSR (3.3 C): linked to the EXECUTABLE, never to Aver.Runtime.Game -- game/src/GameMain.cpp is the
# only translation unit in this tree allowed to name sr::anything (see its own header comment), and
# this link line is what makes that true at the link-graph level rather than by convention alone,
# mirroring sandbox/CMakeLists.txt's identical `if(TARGET Aver.Render.Sr) target_link_libraries(Sandbox
# PRIVATE Aver.Render.Sr) endif()` block for the composition-root reason given there. Optional, like
# every other feature module this target composes elsewhere in this tree: if(TARGET ...) rather than
# if(AVER_MODULE_...), so a tree configured with Aver.Render.Sr entirely absent still configures (the
# same reasoning modules/runtime.game/CMakeLists.txt's own Aver.Render.Voxi block gives).
if(TARGET Aver.Render.Sr)
  target_link_libraries(AverGame PRIVATE Aver.Render.Sr)
endif()

set_target_properties(AverGame PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  # WIN32 above sets /SUBSYSTEM:WINDOWS, which makes the linker look for WinMain. The entry point
  # this engine generates is main(), so point the CRT's startup at the one that calls it.
  set_target_properties(AverGame PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()
