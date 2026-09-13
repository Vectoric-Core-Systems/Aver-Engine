aver_add_module(Aver.Core
  SOURCES
    src/Log.cpp
    src/CrashReport.cpp
    src/Assert.cpp
    src/Time.cpp
    src/Hash.cpp
    src/ErrorCodes.cpp
  PUBLIC_DEFS
    # Version.hpp needs the number the top-level project() declares; keeping it in one place
    # stops a manifest's ENGINE check from being validated against a stale hard-coded copy.
    # AVER_VERSION_SUFFIX ("-1" for 0.6.0-1) is set beside project() because project() only takes
    # numbers; it expands to nothing if this module is ever configured without the root CMakeLists.
    AVER_ENGINE_VERSION="${AverEngine_VERSION}${AVER_VERSION_SUFFIX}"
)
