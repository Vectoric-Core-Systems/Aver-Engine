# tools/

Two kinds of thing live here, and they are not the same kind.

## C++ tools, built by this repo's CMake

Both land in `build/bin` alongside the tests, and neither is a test — they produce reports and files
rather than assertions. They are registered from `tests/formats/CMakeLists.txt` because that is where
their dependencies already are, not because they are tests.

| Tool | What it does |
|---|---|
| `ActorSweep.cpp` | Reports what the actor editor would make of every `.cs` under a directory: how many classes, which are previewable, what each declares. |
| `MakeSamples.cpp` | Synthesises four short `.wav` files — a shot, an impact, a step and a two-note sting. |

```
ActorSweep.exe  "<project>\Content\Scripts"
MakeSamples.exe "<project>\Content\Audio\Source"
```

**`ActorSweep` exists because a fixture agrees with the parser by construction** — the same person
wrote both. Pointed at a real project it found two bugs on its first run that no fixture had:
attribute kinds were searched in turn rather than in file order, so a file naming an `[AverGameMode]`
above an `[AverClass]` came back named after the wrong one; and a file was assumed to declare one
actor, where real ones declare several.

**`MakeSamples` synthesises rather than sources.** A downloaded sample carries somebody's licence into
whatever repository it lands in, and this tree has already made that call: `OcAudioTest` ships no
media and reports a SKIP rather than a pass, on exactly that reasoning. Generated audio has no
licence, is byte-identical on every machine, diffs as source, and is retuned by editing a number.

It emits `.wav`, **not** `.ocaudio`, deliberately — the point is to exercise the editor's import path
end to end, and a generator that wrote the engine's own container would be testing only itself.

## Rust asset pipeline (separate, not built by this CMake)

`aver-assetc` (glTF/OBJ/image import + cook), `aver-ocbeamc` (`.ocbeam` compile), `aver-aerobake`
(wind-tunnel bake), `aver-mapc` (`.scene` → `.ocmap`), `aver-shaderc` (HLSL → DXIL/SPIR-V via DXC).
Standalone executables producing native `.oc*` files.
