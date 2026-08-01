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

# ---------------------------------------------------------------------------------------------------
# avermatc — the material compiler, staged to bin/Tools/.
#
# The editor invokes it after a successful Compile C#, so a .cs material under Content/Materials
# becomes an .ocmat under Binaries/Materials without anybody running a command. A separate directory
# from bin/Scripting because it is a TOOL the editor runs, not an assembly the host loads: nothing
# should be able to reach it by the bridge's probing path.
#
# OUTPUT/DEPENDS rather than POST_BUILD, for the reason stated above the bridge: a POST_BUILD only
# fires when a target relinks, so editing a .cs alone would leave the previous compiler in place with
# a clean build reporting success.
set(AVER_MATC_PROJ ${AVER_CSHARP_DIR}/Aver.MaterialCompiler/Aver.MaterialCompiler.csproj)
set(AVER_MATC_DIR ${CMAKE_BINARY_DIR}/bin/Tools)

file(GLOB AVER_MATC_SOURCES CONFIGURE_DEPENDS
  ${AVER_CSHARP_DIR}/Aver.MaterialCompiler/*.cs
  ${AVER_CSHARP_DIR}/Aver.MaterialCompiler/*.csproj
  ${AVER_CSHARP_DIR}/Aver.Materials/*.cs
  ${AVER_CSHARP_DIR}/Aver.Materials/*.csproj)

set(AVER_MATC_OUTPUTS
  ${AVER_MATC_DIR}/avermatc.dll
  ${AVER_MATC_DIR}/avermatc.runtimeconfig.json
  ${AVER_MATC_DIR}/Aver.Materials.dll)

add_custom_command(
  OUTPUT ${AVER_MATC_OUTPUTS}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_MATC_PROJ} -c Release --nologo -v quiet
          -o ${AVER_MATC_DIR}
  DEPENDS ${AVER_MATC_SOURCES}
  COMMENT "Building the material compiler (staged to bin/Tools)"
  VERBATIM)
add_custom_target(Aver.MaterialCompiler DEPENDS ${AVER_MATC_OUTPUTS})
set_target_properties(Aver.MaterialCompiler PROPERTIES FOLDER "modules")
add_dependencies(Aver.Scripting.Host Aver.MaterialCompiler)

# ---------------------------------------------------------------------------------------------------
# averdesign - the Roslyn reader for an actor script, staged to bin/Tools/ beside avermatc.
#
# Same reasoning as avermatc and the same shape: a TOOL the editor shells out to, not an assembly the
# scripting host loads, so it goes in bin/Tools where the bridge's probing path cannot reach it.
#
# THE ONLY NUGET CONSUMER IN THE TREE. Microsoft.CodeAnalysis is MIT and ships inside the .NET SDK, so
# the restore resolves from the machine's package cache with no network. A machine without it fails
# THIS custom command and nothing else: averDesignAvailable() then reports the tool missing and the
# editor runs with the built-in scanner as its only backend, which is a supported configuration.
set(AVER_DESIGN_PROJ ${AVER_CSHARP_DIR}/Aver.Design/Aver.Design.csproj)
set(AVER_DESIGN_DIR ${CMAKE_BINARY_DIR}/bin/Tools)

file(GLOB AVER_DESIGN_SOURCES CONFIGURE_DEPENDS
  ${AVER_CSHARP_DIR}/Aver.Design/*.cs
  ${AVER_CSHARP_DIR}/Aver.Design/*.csproj)

set(AVER_DESIGN_OUTPUTS
  ${AVER_DESIGN_DIR}/averdesign.dll
  ${AVER_DESIGN_DIR}/averdesign.runtimeconfig.json)

add_custom_command(
  OUTPUT ${AVER_DESIGN_OUTPUTS}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_DESIGN_PROJ} -c Release --nologo -v quiet
          -o ${AVER_DESIGN_DIR}
  DEPENDS ${AVER_DESIGN_SOURCES}
  COMMENT "Building the Roslyn actor-script reader (staged to bin/Tools)"
  VERBATIM)
add_custom_target(Aver.Design DEPENDS ${AVER_DESIGN_OUTPUTS})
set_target_properties(Aver.Design PROPERTIES FOLDER "modules")
add_dependencies(Aver.Scripting.Host Aver.Design)

# ---------------------------------------------------------------------------------------------------
# The F# round trip, staged to bin/FSharpScripts/.
#
# F# NEEDS NO NEW COMPILE MECHANISM. The engine's script compile path is `dotnet build` on a project
# file — here at build time, and ToolsMenu::startCompile at run time — and MSBuild picks the language
# from the project extension. So an .fsproj is driven by the machinery that already exists, and this
# block is the same OUTPUT/DEPENDS shape as the C# samples above for the same POST_BUILD reason.
# (Microsoft.CodeAnalysis, the only Roslyn in this tree, is averdesign's C#-specific READER; it never
# compiled a script, so there was nothing F#-shaped to replace there.)
#
# Only the C# project is named on the command line: it ProjectReferences the .fsproj, so `dotnet build`
# builds both and stages Aver.FSharp.Sample.dll and FSharp.Core.dll alongside it.
#
# bin/FSharpScripts/ rather than bin/Scripts/, for the reason the behaviour and actor samples give: a
# default editor run must not load a demo, and no oracle gate can be made to pick this up.
set(AVER_FSHARP_DIR ${CMAKE_SOURCE_DIR}/scripting/fsharp)
set(AVER_FSHARP_PROJ ${AVER_FSHARP_DIR}/Aver.Scripting.SampleFSharp/Aver.Scripting.SampleFSharp.csproj)
set(AVER_FSHARP_OUT_DIR ${CMAKE_BINARY_DIR}/bin/FSharpScripts)

file(GLOB AVER_FSHARP_SOURCES CONFIGURE_DEPENDS
  ${AVER_FSHARP_DIR}/Aver.FSharp.Sample/*.fs
  ${AVER_FSHARP_DIR}/Aver.FSharp.Sample/*.fsproj
  ${AVER_FSHARP_DIR}/Aver.Scripting.SampleFSharp/*.cs
  ${AVER_FSHARP_DIR}/Aver.Scripting.SampleFSharp/*.csproj)

# FSharp.Core.dll is listed as an OUTPUT deliberately. A library project does NOT copy package assets
# to its output directory by default, so it is possible to get a green build whose scripts folder is
# missing the F# runtime entirely — a failure that only shows up at the first call into F# code.
# Naming it here makes the build itself the thing that notices.
set(AVER_FSHARP_OUTPUTS
  ${AVER_FSHARP_OUT_DIR}/Aver.Scripting.SampleFSharp.dll
  ${AVER_FSHARP_OUT_DIR}/Aver.FSharp.Sample.dll
  ${AVER_FSHARP_OUT_DIR}/FSharp.Core.dll)

add_custom_command(
  OUTPUT ${AVER_FSHARP_OUTPUTS}
  COMMAND ${AVER_DOTNET_EXE} build ${AVER_FSHARP_PROJ} -c Release --nologo -v quiet
          -o ${AVER_FSHARP_OUT_DIR}
  DEPENDS ${AVER_FSHARP_SOURCES}
          ${AVER_BRIDGE_OUTPUTS}
  COMMENT "Building the F# round-trip sample (staged to bin/FSharpScripts)"
  VERBATIM)
add_custom_target(Aver.FSharp.Sample DEPENDS ${AVER_FSHARP_OUTPUTS})
set_target_properties(Aver.FSharp.Sample PROPERTIES FOLDER "modules")
add_dependencies(Aver.Scripting.Host Aver.FSharp.Sample)
