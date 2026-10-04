# AverEngineRuntime.exe -- the standalone runtime, shown as "Aver Engine Runtime".
#
# It was AverGame.exe in game/ until 2026-09-16, when the runtime moved to Runtime/ as the first step of
# splitting the editor from the runtime. The FILE is AverEngineRuntime.exe (no spaces, so no script or
# command line has to quote it); the NAME people see -- Task Manager, file properties, the default
# window title -- is "Aver Engine Runtime", set by AverEngineRuntime.rc the same way sandbox/Sandbox.rc
# makes Sandbox.exe read "Aver Engine".
#
# RESTORED ONCE ALREADY, with the check whose absence is why it was removed. The standalone host and
# the packaged-game staging path went in b262c73 for a stated reason, and it was a real one: a second
# host "rendered a different subset of the scene than the editor", and nothing in the tree could
# notice. The runtime itself never left -- Aver.Runtime.Game kept being built and kept growing, and
# GameApp::onInit is a SUPERSET of the editor's applyProject -- so what was deleted was ~30 lines of glue
# plus three scripts, and roughly 3,500 lines of game host sat compiled and unlinked for nineteen days.
#
# WHAT IS DIFFERENT THIS TIME: scripts/verify-game.ps1 now runs a DIVERGENCE gate. It opens the same
# project and level in both hosts under --frames and compares their probe codes, so "the game draws
# what the editor draws" is a check that fails rather than a claim nobody can test. Restoring the
# executable without that would be restoring the problem.
#
# WIN32 (not a console subsystem): a shipped game that opens a console window behind itself looks
# broken. EntryPoint.hpp generates a standard main(), so the linker is told where to find it rather
# than the source being changed.
#
# This comment used to say "for the same reason Sandbox is", which was never true -- Sandbox was
# console-subsystem until 0.5.0, and the console it opened beside the editor is what finally made
# someone check. It says the reason itself now, rather than pointing at a target that did not share it.
add_executable(AverEngineRuntime WIN32 RuntimeMain.cpp AverEngineRuntime.rc)
target_link_libraries(AverEngineRuntime PRIVATE Aver.Runtime.Game Aver.Runtime Aver.Platform Aver.Core)

# AverSR (3.3 C): linked to the EXECUTABLE, never to Aver.Runtime.Game -- Runtime/host/RuntimeMain.cpp is
# the only translation unit in this tree allowed to name sr::anything (see its own header comment), and
# this link line is what makes that true at the link-graph level rather than by convention alone,
# mirroring sandbox/CMakeLists.txt's identical `if(TARGET Aver.Render.Sr) target_link_libraries(Sandbox
# PRIVATE Aver.Render.Sr) endif()` block for the composition-root reason given there. Optional, like
# every other feature module this target composes elsewhere in this tree: if(TARGET ...) rather than
# if(AVER_MODULE_...), so a tree configured with Aver.Render.Sr entirely absent still configures (the
# same reasoning Runtime/CMakeLists.txt's own Aver.Render.Voxi block gives).
#
# ORDER-DEPENDENT: if(TARGET) is answered when this file is configured, so Runtime/ must be added from
# the root after modules/render.sr. It is -- see the root CMakeLists.txt.
if(TARGET Aver.Render.Sr)
  target_link_libraries(AverEngineRuntime PRIVATE Aver.Render.Sr)
endif()

set_target_properties(AverEngineRuntime PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  FOLDER "Runtime")

# The resource script embeds the icon, but nothing tells the generator that it reads the .ico -- so
# replacing the icon would not re-run the resource compiler and the exe would keep the old one. The same
# fix sandbox/CMakeLists.txt applies to Sandbox.rc.
set_property(SOURCE AverEngineRuntime.rc APPEND PROPERTY
  OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/branding/icon.ico)

if(MSVC)
  # WIN32 above sets /SUBSYSTEM:WINDOWS, which makes the linker look for WinMain. The entry point
  # this engine generates is main(), so point the CRT's startup at the one that calls it.
  set_target_properties(AverEngineRuntime PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()
