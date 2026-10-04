# F# in Aver

The engine runs F#. This directory holds the proof of it and nothing else — no PCG API, no
orchestration layer, no scene bindings. Those depend on this seam, so this seam is settled first.

```
build\bin\Sandbox.exe --headless --frames 30 --scripts FSharpScripts
```

```
[INFO ] [Scripting] managed bridge online (contract v3, 10.0.10, API v1.0.0.0)
[INFO ] [FSharp] Aver.FSharp.Sample v1.0.0.0 (F#-compiled) bound to FSharp.Core v10.1.0.0
[INFO ] [FSharp] load contexts: this assembly in 'AverScripts', Aver.Scripting in 'IsolatedComponentLoadContext(...\bin\Scripting\Aver.Scripting.Bridge.dll)'
[INFO ] [FSharp] round trip OK: RoundTrip.Checksum(20260801, 64) returned 442573848 (0x1A612418) and the separately compiled C# mirror agrees
[INFO ] [Scripting] loaded Aver.Scripting.SampleFSharp.dll: 1 behaviour(s)
[INFO ] [RHI.D3D12] debug layer totals: 0 corruption, 0 error, 0 warning
```

Every one of those lines comes out of the engine's own log. The middle two originate in an assembly
that `fsc` produced.

## How F# compiles: `dotnet build`, not FSharp.Compiler.Service

`dotnet build` on an `.fsproj`. MSBuild picks the language from the project extension, so **F# needed
no new compile mechanism at all** — the two places the engine already compiles scripts both accept an
`.fsproj` unchanged:

| | Where | What it runs |
|---|---|---|
| build time | `modules/scripting/CMakeLists.txt` | `dotnet build <proj> -c Release --nologo -v quiet -p:UseSharedCompilation=false -nodeReuse:false -o <dir>` |
| run time | `ToolsMenu::startCompile` | `dotnet build <proj> -c Release -p:UseSharedCompilation=false -nodeReuse:false --nologo -o <dir>` |

The two rows used to differ in a way nobody had decided: the run-time one passed **no `-c`**, so a
project's own `Scripts.dll` was built Debug while every contract assembly it referenced was built
Release. They now agree. `-p:UseSharedCompilation=false` is on both because `VBCSCompiler.exe`
outlives the build holding its referenced assemblies open, and those are exactly the files the next
build overwrites — it failed a gate run twice. **`-nodeReuse:false` was added to both later, on the
same kind of evidence**: MSBuild's own worker nodes (as opposed to the compiler server the first flag
stops) hold `obj/**/*.dll` open across builds the identical way, and only the full multi-project CMake
build reproduced it — a single-project `dotnet build` spawns no worker nodes at all, so the first pass
through this file saw nothing to fix. The long version of both is in `modules/scripting/CMakeLists.txt`.

**FSharp.Compiler.Service was the alternative and was rejected.** It is the F# analogue of Roslyn and
its licence is fine (MIT), but two things count against it here, both checked rather than assumed:

- **It does not restore offline on this machine.** The SDK's offline `library-packs` folder contains
  exactly one `.nupkg` — `FSharp.Core.<v>.nupkg`. The SDK does ship `FSharp.Compiler.Service.dll`
  (38.6 MB, under `sdk/<version>/FSharp/`) but that is `fsc`'s own copy, not something a
  `PackageReference` resolves. Adding it would make the first build of this tree reach the network.
  *(As first written this bullet named `10.1.302` and `sdk/10.0.302`, and the SDK has since moved to
  10.0.400 / `FSharp.Core.10.1.400`. The version numbers are dropped rather than re-pinned: the
  argument does not turn on them, and a number in prose is a number that goes stale unwatched.)*
  *(It also closed "which no other part of it does" — that was wrong. `Microsoft.CodeAnalysis` did
  reach the network, despite three comments claiming otherwise. It no longer does, because the
  package is now vendored at `third_party/nuget`; see the repo-root `NuGet.config`.)*
- **It means hosting a compiler in a process whose job is to draw frames.**
  `scripting/csharp/Aver.Design/Aver.Design.csproj` already argued that case for Roslyn and settled it
  the other way. Nothing about F# reopens it.

The cost of the choice is honest: a process start per compile, a second or two. That is what C# already
pays, and the F# sample restores and builds in about 1.5 s from cold.

### There was no Roslyn to replace

Worth stating plainly, because the brief for this work assumed otherwise. `Microsoft.CodeAnalysis.CSharp`
appears exactly once in the tree, in `Aver.Design`, and that tool **reads** an actor script to recover
its designer regions when the built-in scanner reports `Malformed`. It has never compiled a script.
The C# scripting path is a `dotnet build` shell-out and always was, which is precisely why F# slots
into it.

### Licences

FSharp.Core is **MIT**, and it is not a new third-party dependency either: the .NET SDK carries it at
`sdk/<version>/FSharp/library-packs/FSharp.Core.<v>.nupkg`, so the restore resolves from the machine
with no network. Re-measured after the repo-root `NuGet.config` cleared every source but
`third_party/nuget`: restoring `Aver.Pcg.fsproj` into an empty packages folder pulls `fsharp.core` and
nothing else, because the SDK injects `library-packs` through the MSBuild property
`RestoreAdditionalProjectSources` rather than as a NuGet source, which is not what `<clear/>` clears.

The installed SDK is pinned by the repo-root `global.json` rather than described here, so this
paragraph no longer names a version to go stale. `dotnet --version` at the repo root is the answer.

## What is here

| Project | Language | Role |
|---|---|---|
| `Aver.FSharp.Sample` | F# | `RoundTrip.Checksum` and `RoundTrip.Describe`. References nothing of the engine's. |
| `Aver.Scripting.SampleFSharp` | C# | An ordinary `AverBehaviour` that calls them. |

**Added since this table was written, and not this seam's proof — the PCG API the section below still
says is "deliberately not started here":** `Aver.Pcg` (F#: `Types.fs`, `Primitives.fs`, `Pcg.fs` —
deterministic hashing and boundary types, staged to `bin/Scripting/` beside the bridge, built by
`modules/scripting/CMakeLists.txt`'s `Aver.Pcg` target), `Aver.Pcg.SampleRules` (F#: `Forest.fs`,
`Sky.fs` — worked generation rules) and `Aver.Scripting.SamplePcg` (C#: `PcgScatterBehaviour`, staged
to `bin/PcgScripts/`, run with `Sandbox.exe --scripts PcgScripts`). See §"What this does not do" below
for what that changes and what it does not.

The C# project's entire F# integration is **one `ProjectReference` to an `.fsproj`**. Nothing on the
native side, in the bridge, or in the host knows which compiler emitted the IL it is loading.

`Aver.FSharp.Sample` deliberately does **not** reference `Aver.Scripting`. The bridge skips an assembly
that references neither `Aver.Scripting` nor `Aver.Framework` (`HostBridge.CheckApiVersion`), which is
the documented behaviour for a support library sitting in a scripts folder — and a support library is
exactly what a future F# PCG assembly will be.

## The check is relational, and it has been shown failing

The same integer recurrence exists twice: in `RoundTrip.fs` compiled by `fsc`, and in
`FSharpRoundTripBehaviour.Mix`/`ChecksumInCSharp` compiled by `csc`. **Exactly one thing differs
between them — the compiler.** Equal answers therefore mean the F# assembly loaded, its FSharp.Core
bound, and its code ran. No baseline, no recorded expected value, nothing to keep in sync.

It is not a check until it has been seen to fail. Changing one constant in `RoundTrip.fs`
(`0x2c1b3c6d` → `0x2c1b3c6e`) and rebuilding only the F# side:

```
seed=20260801 steps=64 fsharp=1896857943 (0x710FC157) csharp=442573848 (0x1A612418) MISMATCH
seed=0        steps=1  fsharp=-1086382832           csharp=350040869            MISMATCH
seed=-7       steps=5  fsharp=-159300080            csharp=765383979            MISMATCH
seed=2147483647 steps=3 fsharp=1231475489           csharp=630050038            MISMATCH
```

and in the engine itself, same command as above:

```
[ERROR] [FSharp] round trip MISMATCH: F# returned 1896857943 (0x710FC157) but the C# mirror computed
        442573848 (0x1A612418) - the two implementations of the same recurrence have diverged
```

The F# side uses a list, `|>`, `List.map`, `List.fold` and `sprintf` on purpose. All of those are
FSharp.Core, so an answer that comes back at all has already proved FSharp.Core is live inside the
host's collectible load context. A pure-arithmetic body would have compiled to IL needing nothing but
the BCL, and would have proved much less.

## Two traps this hit, both of which produce a green build and a dead script

**1. A library does not copy its package assets.** `CopyLocalLockFileAssemblies` defaults to false for
a library, so the first `dotnet build` of the `.fsproj` emitted `Aver.FSharp.Sample.dll` with **no
`FSharp.Core.dll` beside it**. The scripts folder is the host's only source for a script's private
dependencies. Both projects now set it, and `FSharp.Core.dll` is named as a CMake `OUTPUT` so the
build is what notices if it stops arriving.

**2. The collectible load context could not resolve a script's private dependencies at all.** This is
the one real blocker the work found, and it was not F#-specific. `ScriptLoadContext.Load` resolved only
out of the bridge's own load context; a hosted component's context cannot reach the default one, so
there was nowhere else for `FSharp.Core` to come from. Measured, with the file sitting in the same
directory as the assembly that needed it:

```
[ERROR] [FSharp] the round trip could not run: FileNotFoundException: Could not load file or assembly
        'FSharp.Core, Version=10.1.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a'.
```

`ScriptLoadContext` now falls back to probing the scripts directory. **The host context is still tried
first**, and that order is load-bearing — not as an argument, as a measurement. Running the two
statements in the other order:

```
[INFO ] [Scripting] loaded Aver.Scripting.SampleFSharp.dll: 0 behaviour(s)
[WARN ] [Scripting] ...FSharpRoundTripBehaviour has lifecycle-shaped methods but does not derive from
        AverBehaviour, so nothing will call them. Add ': AverBehaviour' and mark the hooks 'override'.
```

— about a class whose declaration is literally `: AverBehaviour`. `dotnet build -o` copies
`Aver.Scripting.dll` into the scripts folder whatever the `.csproj` asks for, so a directory-first probe
loads a second one into the collectible context and `AverBehaviour` stops being itself. Probed assemblies
go into the **collectible** context, so a reload still unloads them.

That change also lifts the same limitation for C#: before it, a C# script assembly could not have any
private dependency either.

### One thing that was claimed and then disproved

The first version of this work cached loaded assemblies by path and asserted the cache was what kept
FSharp.Core to a single identity. It is not. Disabling the cache changed nothing, and a direct probe
showed why: two `LoadFromStream` calls with the same image into one `AssemblyLoadContext` return the
**same** `Assembly` object (`ReferenceEquals` true, one entry in `.Assemblies`). The runtime already
guarantees it. The cache was kept for the only benefit that survived measurement — skipping a second
2.4 MB read of `FSharp.Core.dll` on every load and every hot reload — and the comment now says that and
nothing more. A behaviour-side check that counted those identities was deleted rather than shipped,
because nothing in this repo could have made it fail.

## Staging

`bin/FSharpScripts/`, not `bin/Scripts/` — the same choice `SampleScripts` and `ActorScripts` make, so
a default editor run loads no demo and no oracle gate can be made to pick this up by opening a project.

## What this does not do

**Corrected — the PCG API is no longer future work.** This section used to say "no PCG API... the
next stage depends on this one and is deliberately not started here." `Aver.Pcg` now exists (see "What
is here" above): deterministic hashing, boundary types built for the F#/C# split this seam proved, and
a worked forest-scatter example a `PcgScatterBehaviour` actually runs and spawns entities from. One
part of the original claim still holds, unverified-but-not-contradicted: no F#-authored actor or
behaviour was found (every `AverBehaviour` subclass in the tree, including the PCG sample's, is C#).

**The second F# compile path this section once left unsettled now exists.** `sandbox/src/ProjectScaffold.cpp`'s
`fsprojText()` (added by `e09691a`, "Sky: a project's sky is an F# PCG graph it owns, not numbers in
its level") scaffolds `Scripts.FSharp.fsproj` into a new project's own `Content/Scripts/` directory —
distinct from the checked-in `Aver.Pcg.SampleRules` this seam shipped with — and `Scripts.csproj`
carries a `ProjectReference` to it guarded by `Condition="Exists(...)"`, so a project's own F# rules
(the scaffolded `Sky.fs`) build as part of the ordinary `dotnet build` of `Scripts.csproj`, deleting the
file being the supported way to opt back out to C#-only. `0a9c959` fixed the gap this created for a
project scaffolded before the reference existed. This is now a project-authoring path, not just this
seam's own proof.

An F# type *could* derive from `AverBehaviour` and be discovered directly — nothing in the bridge
prevents it — but that is untested and is not claimed. What is proved is the shape this work was asked
for: **F# compiles, loads, is called from C#, and returns a value the engine logs.**
