# AvDxc.cmake -- a SPIR-V-capable DXC beside the executables, off Windows.
#
#   aver_stage_dxc(<target>)   find libdxcompiler, copy it into ${CMAKE_BINARY_DIR}/bin
#
# On Windows modules/rhi.d3d12/CMakeLists.txt owns this step, with the DLL vendored in
# third_party/dxc-spirv. Elsewhere there is no D3D12 backend to own it, and Microsoft's builds are
# per-platform, so the library is looked for in this order and the first hit wins:
#
#   1. AVER_DXC_LIBRARY        an explicit path, for anything the search below gets wrong
#   2. $VULKAN_SDK/lib         the LunarG SDK ships a SPIR-V-capable DXC on Linux and macOS
#   3. the system library path a distribution package, if there is one
#   4. Microsoft's own release Linux x86_64 only: the tarball pinned below, fetched at configure
#                              time and checked against its SHA-256. FETCHCONTENT_FULLY_DISCONNECTED
#                              and FETCHCONTENT_SOURCE_DIR_AVER_DXC work as for any FetchContent
#                              dependency, so an offline machine can supply the archive itself.
#
# MACOS HAS NO STEP 4: Microsoft publish no macOS build and Homebrew does not package DXC, so there it
# is the Vulkan SDK or nothing. Nothing is fatal at configure time -- the Vulkan backend loads DXC at
# run time and, without it, reports "no shader compiler" exactly as it does on a Windows machine
# with no dxcompiler.dll.
#
# ONLY libdxcompiler IS COPIED. The release also carries libdxil, the DXIL validator; SPIR-V is never
# validated or signed by it, so nothing here loads it (third_party/dxc-spirv/README.md has the same
# rule for dxil.dll on Windows).

set(AVER_DXC_LIBRARY "" CACHE FILEPATH "Path to a SPIR-V-capable libdxcompiler (.so/.dylib); empty = search")

set(_AVER_DXC_LINUX_URL
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz")
set(_AVER_DXC_LINUX_SHA256 "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1")

function(aver_stage_dxc target)
  if(WIN32)
    return()
  endif()
  if(APPLE)
    set(_leaf "libdxcompiler.dylib")
  else()
    set(_leaf "libdxcompiler.so")
  endif()

  set(_lib "")
  set(_from "")
  if(AVER_DXC_LIBRARY)
    if(EXISTS "${AVER_DXC_LIBRARY}")
      set(_lib "${AVER_DXC_LIBRARY}")
      set(_from "AVER_DXC_LIBRARY")
    else()
      message(WARNING "Aver: AVER_DXC_LIBRARY=${AVER_DXC_LIBRARY} does not exist; searching instead")
    endif()
  endif()
  if(NOT _lib AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/lib/${_leaf}")
    set(_lib "$ENV{VULKAN_SDK}/lib/${_leaf}")
    set(_from "the Vulkan SDK")
  endif()
  if(NOT _lib)
    find_library(AVER_DXC_SYSTEM_LIBRARY NAMES dxcompiler)
    if(AVER_DXC_SYSTEM_LIBRARY)
      set(_lib "${AVER_DXC_SYSTEM_LIBRARY}")
      set(_from "the system")
    endif()
  endif()
  if(NOT _lib AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    include(FetchContent)
    FetchContent_Declare(aver_dxc
      URL      "${_AVER_DXC_LINUX_URL}"
      URL_HASH SHA256=${_AVER_DXC_LINUX_SHA256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(aver_dxc)
    if(NOT aver_dxc_POPULATED)
      # Downloaded on the first configure only; later ones reuse the unpacked copy under _deps.
      message(STATUS "Aver: no libdxcompiler installed; using Microsoft's v1.9.2609 Linux release")
      FetchContent_MakeAvailable(aver_dxc)   # the archive has no CMakeLists.txt: this only unpacks it
    endif()
    if(EXISTS "${aver_dxc_SOURCE_DIR}/lib/${_leaf}")
      set(_lib "${aver_dxc_SOURCE_DIR}/lib/${_leaf}")
      set(_from "Microsoft's v1.9.2609 release")
    endif()
  endif()

  if(NOT _lib)
    if(APPLE)
      message(WARNING "Aver: no ${_leaf} found. Install the LunarG Vulkan SDK and set VULKAN_SDK (or "
                      "AVER_DXC_LIBRARY); without it the Vulkan backend has no shader compiler.")
    else()
      message(WARNING "Aver: no ${_leaf} found and none could be fetched. Set AVER_DXC_LIBRARY or "
                      "VULKAN_SDK; without it the Vulkan backend has no shader compiler.")
    endif()
    return()
  endif()

  # The real file, not a symlink chain: a distribution's libdxcompiler.so is usually a link to a
  # versioned name, and copying the link alone would leave bin/ pointing at nothing once moved.
  get_filename_component(_real "${_lib}" REALPATH)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  file(COPY_FILE "${_real}" "${CMAKE_BINARY_DIR}/bin/${_leaf}" ONLY_IF_DIFFERENT)
  message(STATUS "Aver: ${_leaf} from ${_from} copied to bin/ (${_real})")
endfunction()
