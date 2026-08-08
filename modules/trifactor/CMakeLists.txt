# Aver.Trifactor -- the virtualized-geometry cluster builder and LOD DAG.
# docs/VIRTUALIZED_GEOMETRY.md section 7 "Slice 0". Its own module per CORRECTION 3 in the task that
# created it: every optional subsystem in this tree is a switchable module
# (cmake/AvModule.cmake, aver_check_module_dag), and this is cook-time-only tooling that nothing
# else needs to link by default.
#
# Core + Formats only: it reads an already-imported OcMeshData and produces an in-memory cluster/DAG
# model, no RHI, no device, no .NET -- decidable and testable with nothing but a CPU, the same
# reasoning modules/landscape and modules/deform use for the same shape of module.
#
# OFF BY DEFAULT, unlike this tree's other optional modules, which default ON because their
# dependencies already exist in the tree. This one's does not: meshoptimizer (MIT,
# github.com/zeux/meshoptimizer) is not vendored at third_party/meshoptimizer, and per CORRECTION 1
# in this module's task it is the ONLY dependency this feature is allowed -- no METIS, no second
# library, no fallback to configure around. Defaulting AVER_MODULE_TRIFACTOR ON would break
# `aver_build` for every developer who has not vendored it, for a module nothing else links yet.
# Flip it ON explicitly (-DAVER_MODULE_TRIFACTOR=ON) once third_party/meshoptimizer exists.
if(NOT TARGET meshoptimizer)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/meshoptimizer ${CMAKE_BINARY_DIR}/third_party/meshoptimizer)
  else()
    message(FATAL_ERROR
      "Aver.Trifactor (AVER_MODULE_TRIFACTOR=ON) needs meshoptimizer (MIT, "
      "https://github.com/zeux/meshoptimizer) vendored at third_party/meshoptimizer, and it is not "
      "there. This is a hard stop rather than a warning because CORRECTION 1 in this module's task "
      "is explicit that meshoptimizer is the ONLY dependency this feature may use -- there is no "
      "hand-rolled or METIS fallback to configure around, by design (licensing: see "
      "docs/ASSET_IMPORT.md and docs/formats/FORMAT_SPECS.md:13, permissive-only, no patent-bearing "
      "tech). Vendor third_party/meshoptimizer (its own CMakeLists.txt exposing a `meshoptimizer` "
      "target is enough) and reconfigure. Nothing in this agent session fetched it: downloading a "
      "file from an external source is outside what this agent will do unattended, even for a "
      "permissively-licensed dependency -- see the top-of-file comment in src/ClusterBuilder.cpp.")
  endif()
endif()

aver_add_module(Aver.Trifactor
  SOURCES
    src/ClusterBuilder.cpp
    src/ClusterSelect.cpp
    src/ClusterAdapt.cpp
  DEPS
    Aver.Core
    Aver.Formats
    meshoptimizer
  PUBLIC_DEFS
    # Reaches a consumer TU only through the link interface, mirroring AVER_MODULE_PBR/_SCENE
    # (modules/render.pbr/CMakeLists.txt, modules/scene/CMakeLists.txt) -- this is what lets
    # tests/formats/src/ConvertTool.cpp write `#if AVER_MODULE_TRIFACTOR` and have it read as FALSE,
    # not undefined-behaves-as-zero-by-luck, in the default AVER_MODULE_TRIFACTOR=OFF tree.
    AVER_MODULE_TRIFACTOR=1
)
