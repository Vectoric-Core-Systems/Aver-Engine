# AvSdl.cmake -- SDL 3, the window/input/gamepad/audio/file-dialog layer everywhere but Windows.
#
#   include(AvSdl)   defines AVER_SDL3_TARGET (the target to link) off Windows; nothing on Windows
#
# Windows keeps its own Win32 code and never configures SDL. Elsewhere SDL is resolved the way every
# dependency this tree does not vendor is: an installed package first (Homebrew's `sdl3`, a
# distribution's SDL3 3.4+), otherwise the release pinned below, fetched at configure time and checked
# against its SHA-256. FETCHCONTENT_FULLY_DISCONNECTED and FETCHCONTENT_SOURCE_DIR_SDL3 work as for
# any FetchContent dependency, so an offline machine can point at a source tree it already has.
# -DAVER_SDL3_FETCH=ON skips the package search, for a machine whose installed SDL is the wrong one.
#
# SHARED, ON PURPOSE. A static SDL linked into the executable AND into a SHARED engine module (the
# scene, physics and UI seams all link Aver.Platform) would be two SDLs in one process, each with its
# own event queue and its own idea of which windows exist. One libSDL3 in bin/ cannot be that.
#
# zlib licence; see the "Third Party Disclaimers" table in README.md.

if(WIN32)
  return()
endif()

option(AVER_SDL3_FETCH "Always build the pinned SDL 3 release instead of looking for an installed one" OFF)

set(_AVER_SDL3_URL "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz")
set(_AVER_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

set(AVER_SDL3_TARGET "")
if(NOT AVER_SDL3_FETCH)
  find_package(SDL3 3.4 CONFIG QUIET COMPONENTS SDL3-shared)
  if(SDL3_FOUND AND TARGET SDL3::SDL3-shared)
    set(AVER_SDL3_TARGET SDL3::SDL3-shared)
    message(STATUS "Aver: SDL ${SDL3_VERSION} from ${SDL3_DIR}")
  endif()
endif()

if(NOT AVER_SDL3_TARGET)
  include(FetchContent)
  # Only what the engine uses. Everything else is off to keep a first configure short: SDL's test
  # suite and examples, its GPU API (the engine has its own RHI) and the camera subsystem.
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_GPU OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  # SDL TREATS EVERY X11 EXTENSION AS REQUIRED ON LINUX unless its option is switched off, and stops
  # configure on the first one whose headers are missing. They are all optional to SDL itself -- the
  # screensaver inhibit, XInput2's raw mouse, XRandR's display list -- so one the machine lacks is
  # switched off here and said out loud. Forced, because an ON without the headers can only fail.
  # A machine with the full set from SDL's README-linux gets the full SDL.
  if(NOT APPLE)
    set(_aver_sdl_x11_ext
      XCURSOR    X11/Xcursor/Xcursor.h
      XDBE       X11/extensions/Xdbe.h
      XINPUT     X11/extensions/XInput2.h
      XFIXES     X11/extensions/Xfixes.h
      XSYNC      X11/extensions/sync.h
      XRANDR     X11/extensions/Xrandr.h
      XSCRNSAVER X11/extensions/scrnsaver.h
      XSHAPE     X11/extensions/shape.h
      XTEST      X11/extensions/XTest.h)
    set(_aver_sdl_off "")
    while(_aver_sdl_x11_ext)
      list(POP_FRONT _aver_sdl_x11_ext _ext _hdr)
      find_path(_AVER_SDL_HAS_${_ext} "${_hdr}")
      if(NOT _AVER_SDL_HAS_${_ext})
        set(SDL_X11_${_ext} OFF CACHE BOOL "switched off by AvSdl.cmake: ${_hdr} is not installed" FORCE)
        list(APPEND _aver_sdl_off ${_ext})
      endif()
    endwhile()
    if(_aver_sdl_off)
      string(REPLACE ";" " " _aver_sdl_off "${_aver_sdl_off}")
      message(STATUS "Aver: SDL builds without the X11 extension(s) ${_aver_sdl_off} (headers not installed)")
    endif()
  endif()
  FetchContent_Declare(SDL3
    URL      "${_AVER_SDL3_URL}"
    URL_HASH SHA256=${_AVER_SDL3_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  message(STATUS "Aver: building SDL 3.4.18 from source (no installed SDL 3.4+ found)")
  FetchContent_MakeAvailable(SDL3)
  set(AVER_SDL3_TARGET SDL3::SDL3-shared)

  # SDL is somebody else's code, and this tree's -Wall -Wextra (top-level add_compile_options) would
  # otherwise apply to every one of its translation units: hundreds of warnings nobody here can act on.
  get_target_property(_sdl_opts SDL3-shared COMPILE_OPTIONS)
  if(_sdl_opts)
    list(REMOVE_ITEM _sdl_opts -Wall -Wextra)
    set_target_properties(SDL3-shared PROPERTIES COMPILE_OPTIONS "${_sdl_opts}")
  endif()
  set_target_properties(SDL3-shared PROPERTIES FOLDER "third_party")
endif()
