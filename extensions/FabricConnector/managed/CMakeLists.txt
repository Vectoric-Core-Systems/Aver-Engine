# The managed half of the Fabric Connector, built into the extension's own folder (docs/plans/fabric/EXTENSION.md section 3.6):
#
#   Aver.Fabric          -> bin/Extensions/FabricConnector/managed/Aver.Fabric.dll   the runtime the host loads and converted scripts compile against
#   Aver.Fabric.Convert  -> bin/Extensions/FabricConnector/tools/                    the script converter (+ Roslyn); conversion time only, never shipped
#
# Both are OPTIONAL like the scripting bridge: without the .NET SDK nothing here builds, and the extension's manifest then lists a file that is
# not there, so the loader refuses it with that reason. Aver.Fabric.Tests is run by tests/managed.
find_program(AVER_DOTNET_EXE dotnet)
if(NOT AVER_DOTNET_EXE)
  message(STATUS "Fabric Connector: no `dotnet` on PATH - the managed half (Aver.Fabric, Aver.Fabric.Convert) will not be built")
  return()
endif()

# The flags modules/scripting/CMakeLists.txt explains: no compiler server and no reused worker nodes, so nothing holds a file the next build writes.
set(AVER_FABRIC_DOTNET_FLAGS -c Release --nologo -v quiet -p:UseSharedCompilation=false -nodeReuse:false)
set(AVER_FABRIC_MANAGED_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ---- Aver.Fabric ----
# `dotnet build -o` stages the project's whole reference closure (Aver.Framework and what it references) beside the output, so it goes to a private
# folder and only Aver.Fabric.dll is copied into the extension: the engine's assemblies are the host's, not the extension's.
file(GLOB_RECURSE AVER_FABRIC_RUNTIME_SOURCES CONFIGURE_DEPENDS
  ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric/*.cs
  ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric/*.csproj)
list(FILTER AVER_FABRIC_RUNTIME_SOURCES EXCLUDE REGEX "/(obj|bin)/")

set(AVER_FABRIC_RUNTIME_BUILD ${CMAKE_BINARY_DIR}/extensions/FabricConnector/managed)
# The bridge's outputs: Aver.Fabric compiles against the same Aver.Framework, so a re-staged bridge (Framework or its closure changed) rebuilds it, and
# the bridge's own build of the shared project folders finishes first.
get_property(AVER_BRIDGE_OUTPUTS GLOBAL PROPERTY AVER_BRIDGE_OUTPUTS)
# The stamp is the rule's output: copy_if_different leaves an identical Aver.Fabric.dll untouched, and an output older than the bridge's would rebuild every time.
add_custom_command(
  OUTPUT ${AVER_FABRIC_RUNTIME_BUILD}/Aver.Fabric.stamp
  BYPRODUCTS ${AVER_FABRIC_STAGE}/managed/Aver.Fabric.dll
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric/Aver.Fabric.csproj ${AVER_FABRIC_DOTNET_FLAGS}
          -o ${AVER_FABRIC_RUNTIME_BUILD}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AVER_FABRIC_STAGE}/managed
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AVER_FABRIC_RUNTIME_BUILD}/Aver.Fabric.dll ${AVER_FABRIC_STAGE}/managed/Aver.Fabric.dll
  COMMAND ${CMAKE_COMMAND} -E touch ${AVER_FABRIC_RUNTIME_BUILD}/Aver.Fabric.stamp
  DEPENDS ${AVER_FABRIC_RUNTIME_SOURCES} ${AVER_BRIDGE_OUTPUTS}
  COMMENT "Fabric Connector: building Aver.Fabric (-> bin/Extensions/FabricConnector/managed)"
  VERBATIM)
add_custom_target(Aver.Fabric.Managed DEPENDS ${AVER_FABRIC_RUNTIME_BUILD}/Aver.Fabric.stamp)
set_target_properties(Aver.Fabric.Managed PROPERTIES FOLDER "extensions")
# Every dotnet build of the engine's C# projects in modules/scripting shares the projects' obj/ folders with this one; they all finish first.
if(TARGET Aver.Scripting.Host)
  add_dependencies(Aver.Fabric.Managed Aver.Scripting.Host)
endif()
# The property-track animation seam of Aver.Fabric's AnimationPlayer (docs/plans/fabric/G5_PARITY.md, A1): its tests load Aver.Anim.Abi.
if(TARGET Aver.Anim.Abi)
  add_dependencies(Aver.Fabric.Managed Aver.Anim.Abi)
endif()

# ---- Aver.Fabric.Convert ----
# Roslyn 5.6.0 comes from the vendored third_party/nuget folder (NuGet.config), so the restore needs no network. It compiles the converted scripts
# against Aver.Fabric.dll and the engine's assemblies at RUN time (AverAssetC fabric passes the folders), so it has no build dependency on either.
file(GLOB AVER_FABRIC_CONVERT_SOURCES CONFIGURE_DEPENDS
  ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric.Convert/*.cs
  ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric.Convert/GdScript/*.cs
  ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric.Convert/*.csproj)
set(AVER_FABRIC_CONVERT_OUTPUTS
  ${AVER_FABRIC_STAGE}/tools/Aver.Fabric.Convert.dll
  ${AVER_FABRIC_STAGE}/tools/Aver.Fabric.Convert.runtimeconfig.json)
add_custom_command(
  OUTPUT ${AVER_FABRIC_CONVERT_OUTPUTS}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_FABRIC_MANAGED_DIR}/Aver.Fabric.Convert/Aver.Fabric.Convert.csproj ${AVER_FABRIC_DOTNET_FLAGS}
          -o ${AVER_FABRIC_STAGE}/tools
  DEPENDS ${AVER_FABRIC_CONVERT_SOURCES}
  COMMENT "Fabric Connector: building Aver.Fabric.Convert (-> bin/Extensions/FabricConnector/tools)"
  VERBATIM)
add_custom_target(Aver.Fabric.Convert DEPENDS ${AVER_FABRIC_CONVERT_OUTPUTS})
set_target_properties(Aver.Fabric.Convert PROPERTIES FOLDER "extensions")
