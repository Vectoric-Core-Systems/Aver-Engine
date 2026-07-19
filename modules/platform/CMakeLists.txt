aver_add_module(Aver.Platform
  SOURCES
    src/FileSystem.cpp
    src/win32/Win32Window.cpp
  DEPS
    Aver.Core
)

if(WIN32)
  target_link_libraries(Aver.Platform PUBLIC user32)
endif()
