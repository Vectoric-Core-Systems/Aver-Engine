#pragma once
// Platform and compiler shims. Self-detects, so headers are usable outside the CMake build.
#include "Types.hpp"

#if defined(_WIN32) && !defined(AVER_PLATFORM_WINDOWS)
  #define AVER_PLATFORM_WINDOWS 1
#endif

#if defined(_MSC_VER)
  #define AVER_DEBUGBREAK() __debugbreak()
  #define AVER_FORCEINLINE  __forceinline
#else
  #include <csignal>
  #define AVER_DEBUGBREAK() std::raise(SIGTRAP)
  #define AVER_FORCEINLINE  inline
#endif

#define AVER_UNUSED(x) ((void)(x))
