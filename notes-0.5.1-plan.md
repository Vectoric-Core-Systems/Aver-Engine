# 0.5.1 — the plan

Everything here is verified against the source. Each hunt finding was found by one agent and then
handed to a second told to *refute* it; 14 candidates went in, 12 came out. Two were refuted and are
recorded at the bottom so nobody chases them again.

Ordered by what it costs to be wrong about it, not by effort.

---

## 1. Data loss on save — four call sites, one shape

**`std::ofstream(path, ios::trunc)` zeroes the file the moment the stream is constructed**, before a
single serialized byte is written. Every one of these overwrites a real, already-saved user asset in
place. Kill the process, lose the GPU, or fill the disk in that window and the *previous good file*
is gone — not just the unsaved edit.

| file | what it destroys |
|---|---|
| `modules/formats/src/Avr1.cpp:325` | `.ocland` — a sculpted terrain section, via `saveOcLand` |
| `modules/formats/src/OcAnim.cpp:79` | `.ocanim` / `.ocskel` — a clip or skeleton, via the Animation Editor's Save |
| `modules/formats/src/OcMesh.cpp:1196` | `.ocmesh` — any mesh re-saved, e.g. by RelodTool |
| `modules/formats/src/OcRig.cpp:185` | `.ocrig` |

**The fix already exists in the tree.** `aver::platform::writeFileBytesAtomic`
(`FileSystem.hpp:84`, implemented `FileSystem.cpp:188`) is write-to-temp-then-rename, and
`OcSave.cpp`, `OcWorld.cpp`, `OcMat.cpp`, `OcGraph.cpp` and `OcMap.cpp` already route through it for
exactly this hazard. These four never did. It is a drop-in swap with no format or signature change.
`tools/RelodTool.cpp:218` already has a comment noting the hazard, which is how long it has been
known and not fixed.

**Do this first.** It is small, it is mechanical, and it is the only item on the list that can
destroy something a user made.

## 2. A gizmo drag can be repointed mid-drag onto a different entity

`sandbox/src/SandboxApp.cpp:11039` captures `editBefore_` from the selected entity at drag start, but
nothing pins the gesture to it: the per-frame block at `:12872` reads and writes whatever
`selectedXform()` resolves to *this frame*. One screen up, `:12855-12870` dispatches
Undo/Redo/SelectAll/Copy/Paste/Duplicate **with no `!dragging_` gate**.

Repro: two objects, select the one that is *not* first in the Outliner, start dragging its gizmo,
and press Ctrl+A while still holding the button. The gizmo switches to a different object and drags
*that* instead; the undo record then describes an entity that was never dragged.

Fix: pin `dragEntity_`/`dragObjIndex_` at gesture start (both grab sites, `:12794` and `:12832`), and
either gate the Edit-verb dispatch on `!dragging_` or make the drag update and `endTransformEdit()`
operate on the pinned target. Small.

## 3. The render oracle cannot go green, and re-recording will not fix it

Running `gates.ps1` twice against the **same** binary moves `rt` 46,33,28 → 43,31,27 and
`shadow-ms-rt` 27,41,57 → 30,45,63. The partition is exact: **every gate that moves has `rt` in its
name; every gate without it is bit-stable across every run.** GI is stable. Ray tracing is not.

Three explanations tested against the source and **all three refuted** — do not re-run them:

- *The frame-budget controller retunes quality mid-run.* No: `frameBudgetTick` returns early on
  `maxFrames_ != 0 && !frameBudgetForced_` (`SandboxApp.cpp:9328`) and gates always pass `--frames`.
- *The six RT history textures are never cleared, so frame 0 reads GPU garbage.* They genuinely are
  never cleared (`VoxiRenderer.cpp:2884-2912`) — but `rtHistValid_` starts false, is reset on resize,
  and is set true only at the end of a frame (`:3089`); every shader read is gated on it.
- *TLAS instance order varies run to run.* No: instances walk `drawsPrev_`, a `std::vector<Draw>`
  (`VoxiRenderer.hpp:854`), in submission order.

What remains is nondeterminism inside the ray path, most plausibly the driver's acceleration-structure
build, which is not contractually bit-reproducible. **Measure before acting**:
`./scripts/rt-spread.ps1 -Gate rt -Runs 20`, and `-Gate centre` as the control — if a non-RT gate also
moves, this whole partition is wrong.

If it holds, the fix is a **measured** per-channel tolerance in `gates.ps1`'s comparison, scoped to RT
configurations only. The mechanism half-exists: there is already a `FLAKY` verdict that does not count
as a failure, but it demands the retry match *exactly*. The 12 non-RT gates keep exact equality —
loosening those would cost more than the regression the oracle exists to catch. The 3-code tolerance
now in `verify-payload.ps1` was guessed from two samples and needs replacing with the measurement.

## 4. The shipped game is missing pieces the editor has

Two instances of one shape: **the game host's composition root is a subset of the editor's, and
nothing checks that.**

- **Audio never starts.** All four editor audio call sites sit behind `#if AVER_SOUND_EDITOR_AUDIO`,
  defined on exactly one target (`sandbox/CMakeLists.txt:126`). `GameApp.cpp` has no audio calls at
  all, so `aver_audio_init` is never reached and a packaged game is silent — `Audio.Load` succeeds
  into no device. The macro is named after *a tab in the editor*, which is why nobody noticed the
  runtime was excluded. Rename it for the capability, define it on both hosts, mirror
  `SandboxApp.cpp:1853 / :3726 / :6486`.
- **Control rigs never register.** `GameApp::onInit` registers every other runtime-registered
  component (the three Synapse ones at `:1467-1479`) but never calls
  `anim::controlRigSystem().registerComponents(...)` or `.install(...)`. The only call sites in the
  tree are `SandboxApp.cpp:1879-1880`. `Aver.Anim.Scene` is already linked into `Aver.Runtime.Game`,
  so this is a missing two-line call, not a missing dependency. Consequence: `Set Control Rig`
  returns false and every IK/aim rig is inert in a shipped game while working in Play.

**Worth more than either fix:** `verify-game.ps1` should assert these, so the next subsystem the
editor initialises and the game host forgets fails the divergence gate instead of shipping.

## 5. Capabilities that exist but cannot be reached

- **`Hud` cannot draw text or hit-test.** `UiDrawList::addText`, `addHitRect` and `hitTest` are
  implemented, backed by an offline font pipeline, and unit-tested (`UiTest.cpp:126-208`) — and
  `ui_abi.h` exports none of them. Pure ABI plumbing plus a font-handle table; the hard part is done.
  Medium.
- **`aver_pbr_get/set_reflectance` and `_f90`** are exported and implemented
  (`Material.cpp:411-421`) but absent from `Pbr.cs`, which binds every other PBR getter/setter. Four
  `DllImport` lines and two properties. Small.
- **`aver_voxi_get/set_gi_update_interval`** is exported but unbound in `Voxi.cs` — the one native
  knob for the cost that dominates under camera motion, unreachable from a script. `docs/ABI.md:1210`
  already records it as the newest unbound pair. Small.

## 6. A guard that guards nothing

**`MaterialConstants`' three-way mirror test never reads either HLSL file it claims to check.** This
is the same shape as the `VoxiFrame` drift that read a matrix as a cone count — and `MaterialConstants`
grew 112 → 144 → 160 bytes during 0.5.0, which is exactly when a mirror drifts. Medium.

## 7. Lower priority, carried forward

- GI still rebuilds on ~70% of moving ticks. Cause named (handle shape at the frustum boundary);
  candidate fix estimated at **+64 ms** and still unmeasured. Measure before choosing.
- Voxi's GI voxel grid is sized once at init and never rebuilt on a live settings change.
- Vulkan is on by default and not at parity: no ray-hit texturing, mesh-shader pipelines refused, no
  shader blob cache, cascade shadow map never written.
- A mesh still renders with one material in the raster path.
- `materialPanel`'s undo bracket has no ID-namespacing or mouse-up safety net.
- Rotate/Scale gizmos act on the anchor object alone; the open question is the pivot.
- Fluid volumes still collapse.

## Refuted — do not chase

- **`vcvars64.bat`'s "`vswhere.exe` is not recognized"** is benign VS-installer noise
  (`NoDefaultCurrentDirectoryInExePath`); the build genuinely succeeds and a clean configure is not
  at risk. I raised this one myself and it is a false alarm.
- One further candidate was refuted during verification.

## Already fixed, post-0.5.0

`stage-game.ps1`'s `-Compile` still invoked the editor with `& $sandbox`, which after the
`/SUBSYSTEM:WINDOWS` change neither waits nor captures — and because `$LASTEXITCODE` keeps a previous
command's value, it could fall through and package **stale** assemblies rather than failing. Fixed in
`ff05d310`; a sweep confirms no PowerShell native invocation of the editor remains anywhere in
`scripts/` or `tools/`.
