aver_add_module(Aver.Core
  SOURCES
    src/Log.cpp
    src/CrashReport.cpp
    src/Assert.cpp
    src/Time.cpp
    src/Hash.cpp
    src/ErrorCodes.cpp
    src/DynamicLibrary.cpp
    src/CharConv.cpp
    # CpuTiming: the scene walk's CPU-side twin to rhi::GpuTimingReport (see CpuTiming.hpp's own
    # comment). Only the cold path lives here -- the one-time TSC calibration and the report
    # builder -- the hot path (CpuLap/CpuNest) is header-only so it can be inlined at every one of
    # the ~100,000 boundaries a frame it is called from.
    src/CpuTiming.cpp
  PUBLIC_DEFS
    # Version.hpp needs the number the top-level project() declares; keeping it in one place
    # stops a manifest's ENGINE check from being validated against a stale hard-coded copy.
    # AVER_VERSION_SUFFIX ("-1" for 0.7.0-1) is set beside project() because project() only takes
    # numbers; it expands to nothing if this module is ever configured without the root CMakeLists.
    AVER_ENGINE_VERSION="${AverEngine_VERSION}${AVER_VERSION_SUFFIX}"
)

# dlopen lives in libdl on glibc before 2.34 and in libc after; CMAKE_DL_LIBS is whichever this
# toolchain needs (empty on macOS and on new glibc). Windows' LoadLibraryW is kernel32, linked always.
if(NOT WIN32)
  target_link_libraries(Aver.Core PUBLIC ${CMAKE_DL_LIBS})
endif()
