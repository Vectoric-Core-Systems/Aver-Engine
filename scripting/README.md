# scripting/

The C# side of Aver Engine. The native half — the CLR host itself — lives in `modules/scripting/` and carries the full write-up in its README.

| Project | What it is |
|---|---|
| `csharp/Aver.Scripting` | What a **user's** scripts reference: `AverBehaviour`, `Log`, and the P/Invoke bindings for the Voxi and PBR C ABIs. Its assembly **version is a contract** — the host rejects a script assembly built against a different major. |
| `csharp/Aver.Scripting.Bridge` | The managed end of the host. Loaded by hostfxr from next to the executable; owns the collectible load context, reflection discovery and the exception boundary. Never referenced by user scripts. |
| `csharp/Aver.Scripting.SampleBehaviour` | Two behaviours, staged to `build/bin/SampleScripts/`. `HelloBehaviour` proves the boundary works end to end; `GiSwitchBehaviour` proves managed code can change what the editor draws. |
| `csharp/Aver.Scripting.Sample` | A standalone console app that exercises the Voxi/PBR bindings **out of process**. |

## In-process vs out-of-process

Both are real, and the difference is the whole point of the host:

- **In-process** (`build\bin\Sandbox.exe --scripts <dir>`) — the CLR runs inside the editor, so a P/Invoke resolves to the module the editor has **already loaded**. Same settings singleton, real device capabilities.
- **Out-of-process** (`dotnet run --project scripting/csharp/Aver.Scripting.Sample`) — a separate process loads its *own* copy of `Aver.Render.Voxi.dll` and therefore sees default settings and empty caps. Useful for checking the bindings compile and marshal; useless for driving the editor.

`Aver.Scripting.Log` works in both: with a host it reaches the engine's log, without one it falls back to the console.

## Building

The engine's CMake builds and stages the bridge automatically when `dotnet` is on `PATH`, and skips it — with a status message at configure time — when it is not. To build the managed side by hand:

```
dotnet build scripting/csharp/Aver.Scripting.Bridge/Aver.Scripting.Bridge.csproj -c Release -o build/bin
```

## Where a project's scripts come from

A project's own C# lives in `<project>\Content\Scripts` and is built by **Tools ▸ Compile Scripts** into `<project>\Binaries\Scripts` — the one directory the scripting host is pointed at when a project is open. **Tools ▸ Reload Scripts** rebuilds and swaps the result into the running editor.

`--scripts <dir>` still overrides both, which is how the staged sample stays reachable (`--scripts SampleScripts`) and why no oracle gate can be made to load a project's scripts.

Hot reload is implemented, and what it does *not* do is carry state: a behaviour's fields start again from their initialisers on the far side of a swap. See `modules/scripting/README.md`.
