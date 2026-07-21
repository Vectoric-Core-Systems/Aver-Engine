# scripting/

The C# side of Aver Engine. The native half — the CLR host itself — lives in
`modules/scripting/` and carries the full write-up in its README.

| Project | What it is |
|---|---|
| `csharp/Aver.Scripting` | What a **user's** scripts reference: `AverBehaviour`, `Log`, and the P/Invoke bindings for the Voxi and PBR C ABIs. Its assembly **version is a contract** — the host rejects a script assembly built against a different major. |
| `csharp/Aver.Scripting.Bridge` | The managed end of the host. Loaded by hostfxr from next to the executable; owns the collectible load context, reflection discovery and the exception boundary. Never referenced by user scripts. |
| `csharp/Aver.Scripting.SampleBehaviour` | A trivial behaviour, staged to `build/bin/SampleScripts/`. Proof the boundary works end to end. |
| `csharp/Aver.Scripting.Sample` | A standalone console app that exercises the Voxi/PBR bindings **out of process**. |

## In-process vs out-of-process

Both are real, and the difference is the whole point of the host:

- **In-process** (`build\bin\Sandbox.exe --scripts <dir>`) — the CLR runs inside the editor, so a
  P/Invoke resolves to the module the editor has **already loaded**. Same settings singleton, real
  device capabilities.
- **Out-of-process** (`dotnet run --project scripting/csharp/Aver.Scripting.Sample`) — a separate
  process loads its *own* copy of `Aver.Render.Voxi.dll` and therefore sees default settings and
  empty caps. Useful for checking the bindings compile and marshal; useless for driving the editor.

`Aver.Scripting.Log` works in both: with a host it reaches the engine's log, without one it falls
back to the console.

## Building

The engine's CMake builds and stages the bridge automatically when `dotnet` is on `PATH`, and skips
it — with a status message at configure time — when it is not. The scripting host then declines at
run time and the editor runs unchanged. To build the managed side by hand:

```
dotnet build scripting/csharp/Aver.Scripting.Bridge/Aver.Scripting.Bridge.csproj -c Release -o build/bin
```

Hot-reload is the next phase; the collectible load context that makes it possible is already in
place, because it could not have been added afterwards.
