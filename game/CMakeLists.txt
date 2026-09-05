# AverGame.exe -- the standalone game runtime.
#
# WIN32 (not a console subsystem) for the same reason Sandbox is: a shipped game that opens a
# console window behind itself looks broken. EntryPoint.hpp generates a standard main(), so the
# linker is told where to find it rather than the source being changed.
add_executable(AverGame WIN32 src/GameMain.cpp)
target_link_libraries(AverGame PRIVATE Aver.Runtime.Game Aver.Runtime Aver.Platform Aver.Core)
set_target_properties(AverGame PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  # WIN32 above sets /SUBSYSTEM:WINDOWS, which makes the linker look for WinMain. The entry point
  # this engine generates is main(), so point the CRT's startup at the one that calls it.
  set_target_properties(AverGame PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()
