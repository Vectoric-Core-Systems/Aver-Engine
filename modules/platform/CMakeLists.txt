aver_add_module(Aver.Platform
  SOURCES
    src/DirectoryWatcher.cpp
    src/FileSystem.cpp
    src/Image.cpp
    src/InputState.cpp
    src/win32/Win32Window.cpp
    src/win32/Win32Splash.cpp
    src/win32/Win32DirectoryWatcher.cpp
  DEPS
    Aver.Core
)

target_include_directories(Aver.Platform PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)

if(WIN32)
  # shell32/ole32 are for the known-folder lookups and IFileOpenDialog in FileSystem.cpp —
  # Windows SDK only, no third-party dependency added.
  target_link_libraries(Aver.Platform PUBLIC user32 gdi32 shell32 ole32)
endif()
