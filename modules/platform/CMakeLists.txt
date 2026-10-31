# The OS half of every class here is one directory per platform family: src/win32 on Windows,
# src/posix (and src/sdl, for the window, splash and gamepads) everywhere else. The public headers
# and the portable half are shared.
if(WIN32)
  set(_aver_platform_os_sources
    src/win32/Win32Window.cpp
    src/win32/Win32Splash.cpp
    src/win32/Win32Console.cpp
    src/win32/Win32Speech.cpp
    src/win32/Win32DirectoryWatcher.cpp
    src/win32/Gamepad.cpp)
else()
  set(_aver_platform_os_sources
    src/posix/PosixConsole.cpp
    src/posix/PosixSpeech.cpp
    src/sdl/SdlKeys.cpp
    src/sdl/SdlWindow.cpp
    src/sdl/SdlSplash.cpp
    src/sdl/SdlGamepad.cpp
    src/sdl/SdlFileDialog.cpp)
  if(APPLE)
    list(APPEND _aver_platform_os_sources src/posix/FsEventsWatchBackend.cpp)
  else()
    list(APPEND _aver_platform_os_sources src/posix/InotifyWatchBackend.cpp)
  endif()
endif()

aver_add_module(Aver.Platform
  SOURCES
    src/DirectoryWatcher.cpp
    src/FileSystem.cpp
    src/Image.cpp
    # WebP (libwebp), SVG (NanoSVG) and OpenEXR (tinyexr) behind aver::decodeImage; each vendored library
    # is compiled in a translation unit of its own, and the wrappers are ordinary warning-clean code.
    src/ImageWebp.cpp
    src/ImageSvg.cpp
    src/ImageExr.cpp
    src/ThirdPartyNanoSvg.cpp
    src/ThirdPartyTinyExr.cpp
    src/InputState.cpp
    ${_aver_platform_os_sources}
  DEPS
    Aver.Core
)

target_include_directories(Aver.Platform PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
# The two header-only decoders. SYSTEM so their headers do not warn in our wrappers.
target_include_directories(Aver.Platform SYSTEM PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/nanosvg
  ${CMAKE_SOURCE_DIR}/third_party/tinyexr)
# PRIVATE: no public header names a libwebp type, and a static library's link needs travel with it.
target_link_libraries(Aver.Platform PRIVATE webpdecoder)
# Warnings off for the vendored implementation TUs only.
if(MSVC)
  set_source_files_properties(src/ThirdPartyNanoSvg.cpp src/ThirdPartyTinyExr.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
  set_source_files_properties(src/ThirdPartyNanoSvg.cpp src/ThirdPartyTinyExr.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

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
  #
  # sapi: SAPI 5 text to speech (Win32Speech.cpp), in the box with Windows and the SDK; no third-party code.
  target_link_libraries(Aver.Platform PUBLIC user32 gdi32 shell32 ole32 xinput sapi)
endif()

# SDL everywhere else: the window, the splash, gamepads, the file dialog. PRIVATE -- no public header
# here names an SDL type, and nothing above this module links SDL because of it. The Vulkan surface
# and loader cross to Aver.RHI.Vulkan as void* through Window.hpp for exactly that reason.
if(NOT WIN32)
  target_link_libraries(Aver.Platform PRIVATE ${AVER_SDL3_TARGET})
endif()
if(APPLE)
  target_link_libraries(Aver.Platform PRIVATE "-framework CoreServices")   # FSEvents
endif()
