aver_add_module(Aver.Platform
  SOURCES
    src/DirectoryWatcher.cpp
    src/FileSystem.cpp
    src/Image.cpp
    src/InputState.cpp
    src/win32/Win32Window.cpp
    src/win32/Win32Splash.cpp
    src/win32/Win32Console.cpp
    src/win32/Win32DirectoryWatcher.cpp
    src/win32/Gamepad.cpp
  DEPS
    Aver.Core
)

target_include_directories(Aver.Platform PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)

if(WIN32)
  # shell32/ole32 are for the known-folder lookups and IFileOpenDialog in FileSystem.cpp —
  # Windows SDK only, no third-party dependency added.
  #
  # xinput, NOT Xinput9_1_0 -- Gamepad.cpp's own XInputGetState call. Both import libs exist in the
  # Windows 10/11 SDK and both resolve to a DLL that ships inside Windows itself, needing no
  # redistributable: xinput.lib against xinput1_4.dll (in the box since Windows 8) and
  # Xinput9_1_0.lib against xinput9_1_0.dll (in the box since Vista, and the ONLY one of the two that
  # still worked on XP, which is why it exists at all). Nothing here targets anything older than
  # Windows 10, so that XP/Vista compatibility buys nothing, while xinput1_4 is the modern, more
  # capable API (<xinput.h> assumes it by default unless XINPUT_USE_9_1_0 is defined) -- so a later
  # rumble/guide-button/battery feature is a call away instead of a relink.
  target_link_libraries(Aver.Platform PUBLIC user32 gdi32 shell32 ole32 xinput)
endif()
