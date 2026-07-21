# Aver.Scripting.Host

The engine's **in-process CLR host**. It starts .NET inside the editor's own process, so a
P/Invoke from a script resolves to the module the editor has **already loaded** — the same
`Aver.Render.Voxi.dll`, the same settings singleton, the same real device capabilities.

That is the entire reason this module exists. A standalone C# process P/Invoking the engine's DLLs
gets its *own* copy of each: its own settings, empty device caps, and no way to see or change
anything the editor is doing (`docs/STATUS.md` §4d). Hosting the runtime is the fix, and the C ABIs
were already the right shape for it.

Gated by `-DAVER_MODULE_SCRIPTING=OFF`, and the engine builds and runs exactly as before with it off.

## Layering

| Target | Kind | Links | Holds |
|---|---|---|---|
| `Aver.Scripting.Host` | STATIC | `Aver.Core`, `Aver.Platform` | `ScriptHost`, the hostfxr sequence, `scripting_abi.h` |
| `Aver.Scripting.Bridge` | C# | `Aver.Scripting` | the entry points, the collectible load context, the exception boundary |
| `Aver.Scripting` | C# | — | `AverBehaviour`, `Log`, the Voxi/PBR bindings — what a *user's* scripts reference |

**Not the RHI.** Scripting is not a rendering concern, and a link edge to `Aver.RHI` here would be
the same mistake the Voxi/HAL refactor spent twelve steps undoing. Core for the log and the types,
Platform for the executable directory, and nothing else.

**Nothing is linked against nethost or hostfxr either.** Both are resolved with `LoadLibraryW` at
run time. A missing import in the executable's table would fail the *process* at load time — before
any of this code could decline — so a machine with no .NET runtime would not start the editor at
all. That is the one outcome this module is not allowed to produce.

## Declining is the contract

`ScriptHost::init` mirrors `VoxiRenderer::init`: it logs **one** line, records a reason and returns
`false`, and the editor runs exactly as it does with no scripting. Verified by driving each branch,
not by reading the code:

| Made to fail | Line |
|---|---|
| `nethost.dll` removed | `init declined: nethost.dll could not be loaded — the .NET runtime is unavailable` |
| `nethost.dll` replaced with an unrelated DLL | `init declined: nethost.dll exports no get_hostfxr_path` |
| bridge assembly not staged | `init declined: the managed bridge was not staged next to the executable …` |
| runtimeconfig demanding a framework nobody has | `init declined: hostfxr_initialize_for_runtime_config failed (0x80008096) — the framework the bridge targets is not installed` |
| bridge built to a different host contract | `init declined: the staged Aver.Scripting.Bridge.dll speaks a different host contract than this build (host v1) — rebuild the managed side` |

All thirteen oracle gates return their exact raw codes in every one of those runs. Scripting does
not touch rendering, and the gates are how that is kept true.

## The sequence

1. `LoadLibraryW("nethost.dll")` → `get_hostfxr_path()`.
2. `LoadLibraryW(<that path>)` → `hostfxr_initialize_for_runtime_config()` against
   `Aver.Scripting.Bridge.runtimeconfig.json`.
3. `hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)`.
4. Bind four `[UnmanagedCallersOnly]` entry points on `Aver.Scripting.Bridge.HostBridge` —
   `Bootstrap`, `LoadScripts`, `Update`, `Shutdown` — and call `Bootstrap`.

The hostfxr declarations live in `src/ScriptHost.cpp` rather than coming from `nethost.h` /
`hostfxr.h`. Those headers ship in the .NET **host pack**, which only exists on a machine with the
SDK installed, so including them would make the engine unbuildable without .NET — the same property
the run-time path is required to have. The surface is four functions and has been stable since
.NET Core 3.0.

### The host does not export anything

`Bootstrap` receives an `AverScriptHostApi` — a plain-C struct of function pointers, currently just
the log. Managed code calls back through those, never through `DllImport`. A P/Invoke would have to
name the loaded module, which is the *executable* (`Sandbox.exe` today), and that would pin a bridge
assembly shipped with the engine to whichever host happens to embed it.

## The load context is collectible, and that could not have been deferred

User assemblies go into a collectible `AssemblyLoadContext`. An assembly in a non-collectible
context can **never** be unloaded — .NET offers no way back — so hot reload is not something that
can be layered on later. It is decided at the first load or not at all, and it is decided here even
though reload itself lands in the next phase. Assemblies are also loaded from a memory stream, so
the DLL on disk is not locked and a rebuild while the editor is open succeeds.

`ScriptLoadContext.Load` delegates to **the bridge's own load context**, not to the default one.
`load_assembly_and_get_function_pointer` loads a hosted component into an isolated context driven by
its `deps.json`, so `Aver.Scripting` is not in the default context at all — the obvious "return
null and fall through to Default" implementation produced
`Could not load file or assembly 'Aver.Scripting'` with the assembly loaded and sitting next to the
executable. Delegating also guarantees `AverBehaviour` has exactly one runtime identity; a second
copy would leave the discovery test matching nothing, silently.

## Lifecycle — a base class, not an attribute

```csharp
using Aver.Scripting;

public sealed class Spinner : AverBehaviour
{
    public override void OnStart()          => Log.Info("hello from managed code");
    public override void OnUpdate(float dt) { }
    public override void OnShutdown()       { }
}
```

Discovered by reflection on load, constructed through a public parameterless constructor, and
called on the engine's main thread from the frame loop.

**Why the base class.** Both were on the table. An attribute (`[AverScript]`, hooks found by name)
lets a misspelt `OnUpate` compile cleanly and then never run — a failure with no error message
anywhere, which is the worst kind for a layer aimed at people who are not engine developers. The
base class makes the compiler check the hooks, and reduces discovery to one `IsAssignableFrom`. The
cost is C#'s single inheritance, which is acceptable: a behaviour is a leaf type, and shared logic
belongs in something it holds rather than something it inherits.

## A managed exception never crosses back into C++

It cannot be allowed to: an exception escaping an `[UnmanagedCallersOnly]` method does not become a
C++ exception the engine could catch, it **terminates the process**. So every entry point is wrapped
whole, and each `OnStart` / `OnUpdate` / `OnShutdown` call is wrapped individually — per behaviour,
inside the loop, so one throwing behaviour does not stop the ones after it. A behaviour that throws
is logged and **disabled** for the session. Measured:

```
[ERROR] [Scripting] ThrowsOnStart.OnStart threw: InvalidOperationException: deliberate OnStart failure - the behaviour has been disabled
[INFO ] [ThrowsOnUpdate] update 1
[INFO ] [ThrowsOnUpdate] update 2
[ERROR] [Scripting] ThrowsOnUpdate.OnUpdate threw: InvalidOperationException: deliberate OnUpdate failure - the behaviour has been disabled
[INFO ] [Survivor] still running at update 20
```

## Two versioned contracts, checked at their own boundaries

| Boundary | Version | Checked by |
|---|---|---|
| host ↔ bridge | `AVER_SCRIPTING_CONTRACT_VERSION` in `scripting_abi.h`, plus `sizeof` the struct | the bridge, in `Bootstrap` |
| bridge ↔ user assembly | the assembly version of `Aver.Scripting` | the bridge, per loaded assembly |

The second is read out of the user assembly's own **reference table**, not from an attribute the
author has to remember to apply — a check nobody can forget to opt into is the only kind that helps.
An assembly that does not reference `Aver.Scripting` cannot contain a behaviour and is skipped
silently, because a scripts folder legitimately holds support libraries.

```
[ERROR] [Scripting] BadScripts.dll was built against Aver.Scripting 2.0.0.0 but this engine provides 1.0.0.0
        - the assembly was rejected. Rebuild it against this engine.
```

## Staging

The bridge, its `.runtimeconfig.json`, `Aver.Scripting.dll` and `nethost.dll` are staged next to the
executable by `OUTPUT`/`DEPENDS` custom commands — **never** `POST_BUILD`. A `POST_BUILD` rule only
fires when the target relinks, so editing a `.cs` alone would ship the previous bridge with a clean
build reporting success. That exact bug has already shipped a stale splash image in this repo once.

All of it is optional. With no `dotnet` on `PATH` there is nothing to build the bridge with, CMake
says so at configure time and skips the rules, and the host declines at run time — the same path a
user machine with no runtime takes.

## Trying it

```
build\bin\Sandbox.exe --scripts SampleScripts
```

`scripting/csharp/Aver.Scripting.SampleBehaviour` is staged to `bin/SampleScripts/`, deliberately
**not** `bin/Scripts/` (the default), so a normal editor run loads nothing and a demo script does
not run unasked in the product:

```
[INFO ] [Scripting] managed bridge online (contract v1, 10.0.10, API v1.0.0.0)
[INFO ] [HelloBehaviour] OnStart from managed code - hosted in-process on 10.0.10
[INFO ] [Scripting] loaded Aver.Scripting.SampleBehaviour.dll: 1 behaviour(s)
[INFO ] [HelloBehaviour] OnUpdate has run 10 times (0.185s of frame time)
[INFO ] [HelloBehaviour] OnShutdown after 40 update(s)
```

Every one of those lines originates in managed code and reaches the console through the engine's own
log, which is what makes it proof rather than a claim.

## Next

Hot reload — the collectible context exists for it, and what is still missing is a file watcher, a
drain/rebuild/reload step and a way to carry a behaviour's state across the swap. After that:
exposing scene and actor handles to `AverBehaviour` (the C ABIs are shaped for it), and wiring
Tools ▸ Compile Scripts' output directory to the host's scripts directory so a project's scripts
load without a flag. Open items are tracked in `docs/STATUS.md` §4d.
