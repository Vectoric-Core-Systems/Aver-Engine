# Aver.Scripting.Host — in-process CLR host (see README.md).
#
# Core + Platform, and deliberately NOT the RHI: scripting is not a rendering concern, and a link
# edge to Aver.RHI here would be the same mistake the Voxi/HAL refactor spent twelve steps undoing.
aver_add_module(Aver.Scripting.Host
  SOURCES
    src/ScriptHost.cpp
  DEPS
    Aver.Core
    Aver.Platform
  PUBLIC_DEFS
    AVER_MODULE_SCRIPTING=1
)

# nethost/hostfxr are resolved with LoadLibraryW at run time, so nothing is linked here — that is
# what lets this module build on a machine with no .NET at all, and lets the editor start on one.

# ---------------------------------------------------------------------------
# The managed half: the bridge assembly, its runtimeconfig, and nethost.dll.
#
# All of it is OPTIONAL. Without the .NET SDK there is nothing to build the bridge with, so the
# staging rules are skipped, ScriptHost::init finds no bridge next to the executable and declines —
# which is the same path a user machine with no runtime takes. The engine still builds and runs.
# ---------------------------------------------------------------------------
find_program(AVER_DOTNET_EXE dotnet)

if(NOT AVER_DOTNET_EXE)
  message(STATUS "Aver.Scripting.Host: no `dotnet` on PATH - the managed bridge will not be staged, "
                 "and the scripting host will decline at run time")
  return()
endif()

set(AVER_CSHARP_DIR ${CMAKE_SOURCE_DIR}/scripting/csharp)
set(AVER_BRIDGE_PROJ ${AVER_CSHARP_DIR}/Aver.Scripting.Bridge/Aver.Scripting.Bridge.csproj)

# STEP 11 stages the managed bridge into bin/Scripting/, NOT bin/. The bridge now references the gameplay
# assembly Aver.Framework, whose managed DLL — and Aver.Scene's — share a FILE NAME with the NATIVE
# gameplay/world DLLs that already live in bin/. Copying the managed copies into bin/ would overwrite the
# native ones; a subdirectory keeps the two apart. ScriptHost is pointed here by SandboxApp (bridgeDir =
# <exe>/Scripting), and each managed contract assembly redirects its own P/Invoke back to bin/ through a
# DllImportResolver (see NativeResolver.cs). nethost.dll stays in bin/ (loaded by bare name from the exe).
set(AVER_BRIDGE_DIR ${CMAKE_BINARY_DIR}/bin/Scripting)

# The bridge and every source that compiles INTO it (it pulls in Aver.Framework + Aver.Scene), so a
# clean build after editing any one of them re-stages the bridge. Driven by an OUTPUT/DEPENDS rule and NOT
# by POST_BUILD: a POST_BUILD command only runs when the target actually relinks, so editing a .cs alone
# would ship the previous bridge with a clean build reporting success. That bug has shipped a stale splash
# here once.
#
# GLOBBED, not enumerated. The list used to be written out by hand, and a hand-written list of sources is
# only correct until the next file is added: a new .cs is then invisible to the dependency scan, the
# bridge is never re-staged, and the HOST goes on loading the previous assembly while the project's
# scripts compile happily against the new one. The failure surfaces far from its cause, as a runtime
# MissingMethodException for a member that plainly exists in the source -- which is exactly how adding
# Physics.cs and EnhancedInput.cs broke this. CONFIGURE_DEPENDS makes CMake re-glob when the directories
# change, so adding a file cannot silently fall out of the build again.
file(GLOB AVER_BRIDGE_SOURCES CONFIGURE_DEPENDS
  ${AVER_CSHARP_DIR}/Aver.Scripting.Bridge/*.cs
  ${AVER_CSHARP_DIR}/Aver.Scripting.Bridge/*.csproj
  ${AVER_CSHARP_DIR}/Aver.Scripting/*.cs
  ${AVER_CSHARP_DIR}/Aver.Scripting/*.csproj
  # Aver.Framework (the gameplay layer the bridge drives) and its own dependency Aver.Scene.
  ${AVER_CSHARP_DIR}/Aver.Framework/*.cs
  ${AVER_CSHARP_DIR}/Aver.Framework/*.csproj
  ${AVER_CSHARP_DIR}/Aver.Scene/*.cs
  ${AVER_CSHARP_DIR}/Aver.Scene/*.csproj)

set(AVER_BRIDGE_OUTPUTS
  ${AVER_BRIDGE_DIR}/Aver.Scripting.Bridge.dll
  ${AVER_BRIDGE_DIR}/Aver.Scripting.Bridge.runtimeconfig.json
  ${AVER_BRIDGE_DIR}/Aver.Scripting.dll
  ${AVER_BRIDGE_DIR}/Aver.Framework.dll
  ${AVER_BRIDGE_DIR}/Aver.Scene.dll)

add_custom_command(
  OUTPUT ${AVER_BRIDGE_OUTPUTS}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_BRIDGE_PROJ} -c Release --nologo -v quiet
          -o ${AVER_BRIDGE_DIR}
  DEPENDS ${AVER_BRIDGE_SOURCES}
  COMMENT "Building the managed scripting bridge (staged to bin/Scripting)"
  VERBATIM)
add_custom_target(Aver.Scripting.Bridge DEPENDS ${AVER_BRIDGE_OUTPUTS})
set_target_properties(Aver.Scripting.Bridge PROPERTIES FOLDER "modules")
add_dependencies(Aver.Scripting.Host Aver.Scripting.Bridge)

# A sample behaviour, staged into bin/SampleScripts/ rather than bin/Scripts/ so it is NOT loaded
# by a default editor run. It exists to prove the boundary end to end (`--scripts SampleScripts`
# makes a managed OnStart write a line through the engine log), and a demo script that runs
# unasked in the shipped editor would be noise in the product.
set(AVER_SAMPLE_PROJ ${AVER_CSHARP_DIR}/Aver.Scripting.SampleBehaviour/Aver.Scripting.SampleBehaviour.csproj)
set(AVER_SAMPLE_OUT ${CMAKE_BINARY_DIR}/bin/SampleScripts/Aver.Scripting.SampleBehaviour.dll)
add_custom_command(
  OUTPUT ${AVER_SAMPLE_OUT}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_SAMPLE_PROJ} -c Release --nologo -v quiet
          -o ${CMAKE_BINARY_DIR}/bin/SampleScripts
  DEPENDS ${AVER_SAMPLE_PROJ}
          ${AVER_CSHARP_DIR}/Aver.Scripting.SampleBehaviour/HelloBehaviour.cs
          ${AVER_CSHARP_DIR}/Aver.Scripting.SampleBehaviour/GiSwitchBehaviour.cs
          ${AVER_BRIDGE_OUTPUTS}
  COMMENT "Building the sample script assembly"
  VERBATIM)
add_custom_target(Aver.Scripting.SampleBehaviour DEPENDS ${AVER_SAMPLE_OUT})
set_target_properties(Aver.Scripting.SampleBehaviour PROPERTIES FOLDER "modules")
add_dependencies(Aver.Scripting.Host Aver.Scripting.SampleBehaviour)

# A sample ACTOR assembly, the counterpart of the behaviour sample above, staged to bin/ActorScripts/ so
# it is NOT loaded by a default editor run. It exists to prove the step-11 actor loop end to end:
#   Sandbox.exe --headless --frames 5 --scripts ActorScripts --spawn-test BP_TestActor
# spawns a declared managed actor and ticks it, so OnBeginPlay / OnTick reach the engine log. Depends on
# the bridge outputs so the run has a bridge that speaks the actor dispatch.
set(AVER_ACTOR_PROJ ${AVER_CSHARP_DIR}/Aver.Framework.SampleActor/Aver.Framework.SampleActor.csproj)
set(AVER_ACTOR_OUT ${CMAKE_BINARY_DIR}/bin/ActorScripts/Aver.Framework.SampleActor.dll)
add_custom_command(
  OUTPUT ${AVER_ACTOR_OUT}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_ACTOR_PROJ} -c Release --nologo -v quiet
          -o ${CMAKE_BINARY_DIR}/bin/ActorScripts
  DEPENDS ${AVER_ACTOR_PROJ}
          ${AVER_CSHARP_DIR}/Aver.Framework.SampleActor/TestActor.cs
          ${AVER_CSHARP_DIR}/Aver.Framework.SampleActor/SphereActor.cs
          ${AVER_CSHARP_DIR}/Aver.Framework.SampleActor/PlayDemo.cs
          ${AVER_BRIDGE_OUTPUTS}
  COMMENT "Building the sample actor assembly"
  VERBATIM)
add_custom_target(Aver.Framework.SampleActor DEPENDS ${AVER_ACTOR_OUT})
set_target_properties(Aver.Framework.SampleActor PROPERTIES FOLDER "modules")
add_dependencies(Aver.Scripting.Host Aver.Framework.SampleActor)

# nethost.dll, copied out of the .NET host pack. It is loaded by name at run time, so it has to sit
# next to the executable; the natural-sorted last entry is the newest pack, and any nethost can
# locate any hostfxr. If no pack is installed the copy is skipped and the host declines — the same
# outcome as a machine with no runtime, which is exactly the case that has to keep working.
file(GLOB AVER_NETHOST_CANDIDATES
     "$ENV{ProgramFiles}/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/*/runtimes/win-x64/native/nethost.dll")
if(AVER_NETHOST_CANDIDATES)
  list(SORT AVER_NETHOST_CANDIDATES COMPARE NATURAL)
  list(GET AVER_NETHOST_CANDIDATES -1 AVER_NETHOST_DLL)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bin/nethost.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AVER_NETHOST_DLL} ${CMAKE_BINARY_DIR}/bin/nethost.dll
    DEPENDS ${AVER_NETHOST_DLL}
    COMMENT "Staging nethost.dll"
    VERBATIM)
  add_custom_target(Aver.Scripting.NetHost DEPENDS ${CMAKE_BINARY_DIR}/bin/nethost.dll)
  set_target_properties(Aver.Scripting.NetHost PROPERTIES FOLDER "modules")
  add_dependencies(Aver.Scripting.Host Aver.Scripting.NetHost)
else()
  message(STATUS "Aver.Scripting.Host: no Microsoft.NETCore.App.Host.win-x64 pack found - nethost.dll "
                 "will not be staged, and the scripting host will decline at run time")
endif()
