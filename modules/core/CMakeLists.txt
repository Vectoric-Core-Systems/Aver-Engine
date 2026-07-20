aver_add_module(Aver.Core
  SOURCES
    src/Log.cpp
    src/Assert.cpp
    src/Time.cpp
    src/Hash.cpp
  PUBLIC_DEFS
    # Version.hpp needs the number the top-level project() declares; keeping it in one place
    # stops a manifest's ENGINE check from being validated against a stale hard-coded copy.
    AVER_ENGINE_VERSION="${AverEngine_VERSION}"
)
