# tools/

**This file used to describe two kinds of thing here; there are actually three, and one of the two
original kinds was fictional.** The table below used to list only `ActorSweep` and `MakeSamples`, and
a "Rust asset pipeline" section named five more (`aver-assetc`, `aver-ocbeamc`, `aver-aerobake`,
`aver-mapc`, `aver-shaderc`) as living outside this CMake. There is no Rust anywhere in this
tree — no `.rs` file, no `Cargo.toml` — and none of those five names exists in any form. That section
is gone rather than corrected in place, per `docs/STALE_CODE.md`'s own finding about this file.

## C++ tools, built by this repo's CMake

All of these land in `build/bin` alongside the tests, and none is itself a test — each produces
reports or files rather than assertions. Most are registered from `tests/formats/CMakeLists.txt`,
because that is where their dependencies already are; `DumpClusterPs` is the exception, registered
from `tests/editor/CMakeLists.txt` for the same reason.

| Tool | What it does |
|---|---|
| `ActorSweep.cpp` | Reports what the actor editor would make of every `.cs` under a directory: how many classes, which are previewable, what each declares. |
| `MakeSamples.cpp` | Synthesises four short `.wav` files — a shot, an impact, a step and a two-note sting. |
| `MakeFoliage.cpp` | Synthesises procedural foliage — a pine, an oak and a bush — as `.ocmesh` (and the same geometry as `.obj`) plus the PNG texture set they share, because marketplace foliage cannot live in a permissively-licensed tree and this engine has no mesh authoring of its own. |
| `MakeRig.cpp` | Writes `content/dev/Rig.gltf`, the tree's only skinned asset source, and checks it — the tree had never contained a skinned mesh (no `.gltf`, no `.glb`, no `.ocmesh` with skin streams) before this existed, so nothing had exercised the importer, the container or a human opening one. |
| `AverAssetC.cpp` | The standalone asset compiler the launcher's Aver Exchange feature invokes out of process — built FROM `ConvertTool.cpp`'s merge algorithm and verify discipline (not on top of it), because a caller across a process boundary needs a parseable result and formats beyond glTF. Registered as a shipping tool, unlike `ConvertTool` and `ActorSweep`. |
| `AverCrashReporter.cpp` | The separate process that displays a crash report. Links nothing from the engine — not `Aver.Core`, not the RHI, not one header — because a reporter sharing a binary with the thing that crashed shares its heap, its static initialisers and its bugs. |
| `RelodTool.cpp` | Reports the LOD ladder Trifactor would build for an `.ocmesh` today, against the one the file already carries, and (opt-in, `--write`) writes re-cooked copies elsewhere — because a cooked mesh's ladder is derivable from LOD 0 alone, and re-deriving it used to require the original source asset, which a shipped-cooked project may no longer have. |
| `DumpClusterPs.cpp` | Prints the exact composed HLSL (and `-D` list) the cluster path's pixel shader compiles from, so it can be handed to `dxc` and checked without running the engine — because a shader here is a string composed at runtime from two preludes and a feature's own source, and only `createShader` inside a live device ever type-checks the result. |

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

## Python scripts (not built by CMake at all — a third kind)

`make_pbr_textures.py` generates tileable procedural PBR texture sets (diffuse/ARM/normal) as PNGs
for any project's content folder, for the same licensing reason `MakeFoliage` generates rather than
sources. `mcp/aver_mcp.py` is an MCP server that drives the two hosts — `Sandbox.exe` and
`AverEngineRuntime.exe` — by their existing CLI flags: build, launch, screenshot, crop, and run the
suites through CTest. It turns the manual "open it and look" loop that found most of this tree's
visual defects into tools an agent can call, and it changes nothing in the engine itself. See
`tools/mcp/README.md` for that one.

## What actually compiles the `.oc*` formats

There is no separate compiler toolchain for them, Rust or otherwise. `.ocmesh`, `.ocmat`, `.ocskel`
and `.ocanim` each have a C++ reader/writer under `modules/formats/`; glTF and OBJ import goes through
`modules/formats/src/GltfImport.cpp` / `ObjImport.cpp`; and `.ocmat` specifically is produced by
`avermatc`, a C# tool reflecting a built `Scripts.dll` (see `docs/EDITOR.md` Phase 9). `docs/formats/
DECISIONS.md` has the current status of every `.oc*` extension.
