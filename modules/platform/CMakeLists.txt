aver_add_module(Aver.Platform
  SOURCES
    src/FileSystem.cpp
    src/win32/Win32Window.cpp
    src/win32/Win32Splash.cpp
  DEPS
    Aver.Core
)

target_include_directories(Aver.Platform PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)

if(WIN32)
  target_link_libraries(Aver.Platform PUBLIC user32 gdi32)
endif()
