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
| bridge built to a different host contract | `init declined: the staged Aver.Scripting.Bridge.dll speaks a different host contract than this build (host v3) — rebuild the managed side` |

All thirteen oracle gates return their exact raw codes in every one of those runs. Scripting does
not touch rendering, and the gates are how that is kept true.

## The sequence

1. `LoadLibraryW("nethost.dll")` → `get_hostfxr_path()`.
2. `LoadLibraryW(<that path>)` → `hostfxr_initialize_for_runtime_config()` against
   `Aver.Scripting.Bridge.runtimeconfig.json`.
3. `hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)`.
4. Bind five `[UnmanagedCallersOnly]` entry points on `Aver.Scripting.Bridge.HostBridge` —
   `Bootstrap`, `LoadScripts`, `UnloadScripts`, `Update`, `Shutdown`, and the three HUD
   entries `HudCount` / `HudName` / `HudDraw` — then call `Bootstrap`.

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
can be layered on later. It is decided at the first load or not at all. Assemblies are also loaded
from a memory stream, so the DLL on disk is not locked and a rebuild while the editor is open
succeeds.

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

## HUDs get their own three entries (contract v3)

A HUD is neither a behaviour nor an actor. It has no lifecycle, so `Update` never reaches it, and no
transform or class row, so none of the actor discovery finds it. Before v3 there was no way to call
one at all: `aver_fw_spawn_preview` works only because an actor is a registered class with a vtable
slot.

`[AverHud]` (in `Aver.UI`) marks a class the bridge should discover at load. The contract is short on
purpose — a public parameterless constructor and a `public void Draw(float dt)`. The attribute
carries DATA only and never names the hook, which is the same rule `Aver.Framework/Attributes.cs`
settles: `Draw` is found by SIGNATURE, and a marked class without one is reported by name at load
rather than left to silently never draw.

`Draw(float dt)` and nothing more is the load-bearing decision. A HUD whose signature demanded its
game's state — `Draw(dt, shots, hits, recoil, cooldown)`, which is what SkyForge's had — can only
ever be called by that game, which is exactly the editor's problem. State goes on the instance:
gameplay writes the fields, the editor writes none and gets the defaults.

Discovery matches the attribute **by name**, not by type. A HUD assembly resolves `Aver.UI` out of
its own load context, so a typed comparison would compare `Type` objects from two contexts and match
nothing at all — the same reason `WarnAboutNearMisses` matches the gameplay attributes by name.

A HUD that throws is **disabled**, not retried. `Draw` runs once a frame, so a faulting one would
otherwise write the same stack trace sixty times a second into the Output Log.

The three entries are bound **separately** from the five above and are allowed to fail: a bridge
predating them is already refused by the contract check, and a host that declined to start because an
editor-facing entry point was missing would be refusing to run somebody's game over a preview
feature.

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

## Hot reload

**Tools ▸ Reload Scripts** rebuilds the open project's `Scripts.csproj` and swaps the result into
the running editor. The sequence is three steps and each one belongs where it is:

```
UnloadScripts()   managed  OnShutdown on everything live, drop the list, unload the ALC
dotnet build      native   off-thread, into <project>\Binaries\Scripts
LoadScripts(dir)  managed  fresh collectible context, discover, construct, OnStart
HudCount()        managed  how many [AverHud] classes the loaded assemblies declared
HudName(i,buf,n)  managed  its display name, UTF-8, into a caller-owned buffer
HudDraw(i, dt)     managed  call its Draw(dt); 1 if it ran
```

The **rebuild is native** because the host already owns the `dotnet build` shell-out, and a managed
side spawning compilers would be doing a job it has no business knowing about. The **swap is on the
main thread**: `OnShutdown` and `OnStart` are behaviour hooks, and behaviours are a main-thread
thing, so the build thread only ever sets a flag that the frame loop reaps.

**A failed build does not unload.** Unloading first would leave the editor with no scripts at all
because of a typo — strictly worse than carrying on with the ones already running. The compiler's
whole transcript goes into the modal; the exit code alone never says which line it objected to.

**Unloading is a request, not a command.** A collectible context is only gone once every reference
to anything in it is dropped and a GC has run, so `UnloadScripts` returns 1 for "collected" and 0
for "still finalising" — **both are success**. Nothing waits: the collect loop is bounded at two
cycles, because a behaviour that parked a reference somewhere the engine still holds would keep the
old context alive forever and blocking the editor's main thread on that turns a leak into a hang.
A 0 is reported as a warning naming what it costs (memory), and the new scripts are live regardless.

`DrainAndUnload` is deliberately `[MethodImpl(MethodImplOptions.NoInlining)]`. The context can only
be collected once no stack frame holds a reference to it, and a JIT that inlined it into the caller
would keep the local alive for the whole calling frame — so the collect would report a leak that
only the inlining had created.

**No state is carried across.** A behaviour's fields start again from their initialisers. Carrying
them needs a serialisation contract, and inventing one before the scene layer exists would fix the
shape of something that has no owner yet.

Verified by measurement, in one process, with the `.cs` edited on disk between the two:

```
[INFO ] [Heartbeat] VERSION A - started
[INFO ] [Scripting] loaded Scripts.dll: 1 behaviour(s)
[INFO ] [Heartbeat] VERSION A - update 400
[INFO ] [Editor] Reload Scripts: ...\SkyForge\Content\Scripts\Scripts.csproj built cleanly
[INFO ] [Heartbeat] VERSION A - shutting down after 564 update(s)
[INFO ] [Heartbeat] VERSION B - started, and this class did not exist when the editor launched
[INFO ] [Scripting] loaded Scripts.dll: 1 behaviour(s)
[INFO ] [Editor] Reload Scripts: 1 behaviour(s) live from ...\SkyForge\Binaries\Scripts
[INFO ] [Heartbeat] VERSION B - update 600
```

## Where the host looks for scripts

In priority order:

| | Directory |
|---|---|
| `--scripts <dir>` | absolute, or relative to the executable |
| a project is open | `<project>\Binaries\Scripts` |
| otherwise | `<exe>\Scripts`, which a clean build does not create |

`<project>\Binaries\Scripts` is a function in the editor (`scriptsBinaryDir`) and is passed to
`dotnet build -o`, rather than being an `OutputPath` in the generated `.csproj`. Both ends have to
agree and only one of them is ours to edit: a project scaffolded before this existed would
otherwise build somewhere the host does not look, with nothing anywhere saying why.

The override wins so the staged sample stays reachable with a project open — and so **no oracle
gate can be made to load a project's scripts** by opening one.

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
[INFO ] [Scripting] managed bridge online (contract v3, 10.0.10, API v1.0.0.0)
[INFO ] [GiSwitch] global illumination is Off at startup (status: Ready)
[INFO ] [HelloBehaviour] OnStart from managed code - hosted in-process on 10.0.10
[INFO ] [Scripting] loaded Aver.Scripting.SampleBehaviour.dll: 2 behaviour(s)
[INFO ] [GiSwitch] set global illumination to High from managed code at update 5 - the viewport changes on the next frame
[INFO ] [HelloBehaviour] OnUpdate has run 10 times (0.191s of frame time)
[INFO ] [Sandbox] probe (1375,819) px (0.41,0.36,0.41) raw (104,91,104) ... in-viewport
[INFO ] [GiSwitch] leaving global illumination at High
[INFO ] [HelloBehaviour] OnShutdown after 40 update(s)
```

Every one of those lines originates in managed code and reaches the console through the engine's own
log, which is what makes it proof rather than a claim. **The probe in the middle is the strongest
line there.** A default run of that scene reads `raw(90,93,108)`; `raw(104,91,104)` is the engine's
own `--gi` oracle value, bit for bit — so managed code did not merely log, it changed what the GPU
drew, and the change is verifiable at the raw 8-bit code rather than by eye.

## What a script can and cannot reach

**Can:** `Log`, and the render modules' live settings and this machine's real device capabilities
through `Voxi` and `Pbr`. Because the CLR is in-process those P/Invokes resolve to the modules the
editor has already loaded — same settings singleton, same frame.

**Cannot:** the scene. There are no actor, transform, component, input or asset APIs. That waits on
the generic scene layer (`docs/STATUS.md` §9.1) and is deliberately not stubbed: an interim object
model invented here would be exactly the throwaway ABI that design exists to avoid, and every
script written against it would have to be rewritten.

## Next

Exposing scene and actor handles to `AverBehaviour`, once §9.1 lands and there is something real to
expose. Smaller, and independent: a file watcher so an edit reloads without the menu item, and a
serialisation contract if state should survive a swap. Open items are tracked in
`docs/STATUS.md` §4d.
