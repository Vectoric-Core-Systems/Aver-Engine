# The managed suites that were run only by hand: Aver.Graph.Tests (Aver Node compiler, nodes, host),
# Aver.DataLoc.Tests (data tables, localisation) and Aver.Fabric.Tests (the Godot-compatible runtime). All exit with their failure count, the same contract as
# GasManagedTests (tests/gas). Graph.Tests resolves fixtures from its own folder, so it runs there.
# Not registered without a .NET SDK on PATH (a skip, never a failure).
#
# GRAPH TESTS ON WINDOWS ONLY, for now. 36 of its 433 checks prove that compiled IL issues a real
# P/Invoke by catching EntryPointNotFoundException naming the native symbol -- which only Windows
# produces: there the MANAGED Aver.Scene.dll in the test's folder loads as a native image and lacks the
# export. Elsewhere a managed assembly cannot be dlopen'ed, so the same call fails one step earlier as
# DllNotFoundException and those checks fail although the IL is right. The other 397 pass on Linux; the
# suite comes back everywhere once those assertions accept either exception.
find_program(AVER_DOTNET dotnet)
if(AVER_DOTNET)
  if(WIN32)
    add_test(NAME GraphManagedTests
      COMMAND "${AVER_DOTNET}" run --configuration Release --nologo
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests")
  endif()
  add_test(NAME DataLocManagedTests
    COMMAND "${AVER_DOTNET}" run --project "${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.DataLoc.Tests/Aver.DataLoc.Tests.csproj"
            --configuration Release --nologo
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  # Aver.Fabric (the Godot-compatible runtime): math, Variant, signals, node tree and lifecycle, run by the managed node host.
  add_test(NAME FabricManagedTests
    COMMAND "${AVER_DOTNET}" run --project "${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Fabric.Tests/Aver.Fabric.Tests.csproj"
            --configuration Release --nologo
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set(_aver_managed_suites DataLocManagedTests FabricManagedTests)
  if(WIN32)
    list(APPEND _aver_managed_suites GraphManagedTests)
  endif()
  set_tests_properties(${_aver_managed_suites} PROPERTIES
    TIMEOUT 900
    ENVIRONMENT "DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1"
    FAIL_REGULAR_EXPRESSION "FAIL: |FAIL  ")
  # Each `dotnet run` builds Aver.Framework into the same obj/ (GasManagedTests too, tests/gas): one at a time.
  set_tests_properties(${_aver_managed_suites} PROPERTIES RESOURCE_LOCK aver_managed_build)
else()
  message(STATUS "Managed suites skipped: no dotnet on PATH")
endif()
