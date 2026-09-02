# Stale code sweep — 2026-08-03

A point-in-time sweep at `8fcfd71`, with every finding put to an adversarial verifier. **This is a
work list, not a verdict.** Read the caveat before acting on any line of it.

## What was run

One finder swept the tree (excluding `build*/`, `.git/`, `.claude/worktrees/`, `third_party/`) and
returned **71 candidates**. Every one was then handed to a separate verifier prompted to *refute* it
— to default to "the claim is wrong" and go looking for a caller, a cross-language use through the
ABI, a CMake-conditional build, a virtual override, or a registration table.

| | |
|---|---|
| Candidates found | **71** (the finder's own count said 69 — it miscounted its own list) |
| Verified | **63** |
| → confirmed dead | **45** |
| → defended (a real use was found) | **18** |
| **Unverified** | **8** — those verifiers hit a session limit. **Unverified is not clean.** |

The previous sweep silently capped verification at 12 of 43 candidates and was reported as complete.
This one verifies every listed finding and reports the shortfall explicitly.

---

## The caveat that matters most

**The verifiers contradicted each other on where "unused" ends and "deliberate public API" begins.**

| | |
|---|---|
| `Json.hpp:25` `isBool()` | **defended** — "deliberately public API surface" |
| `Json.hpp:26` `isNumber()` | **confirmed dead** |
| `FrameworkAbi.cpp:314` `aver_fw_scene_abi_version()` | **defended** — "deliberate versioned C ABI export" |
| `FrameworkAbi.cpp:309` `aver_fw_abi_version()` | **confirmed dead** |

Adjacent declarations, identical situations, opposite verdicts. Nobody told the verifiers where that
line sits, and it is the crux for a large share of the 28 `dead-function` findings — most of which
are public accessors on public headers.

**So: the 45 is not 45 deletions.** Roughly a third of it is "an accessor nobody calls yet", which is
a design question, not a defect. The categories below are ordered by how safe they are to act on.

---

## A. Real defects — a variable is written and never read (7 at the time of the sweep; re-verified
2026-09-02 and only 2 are still live)

These cannot be defended as API surface. Each one means a feature silently does nothing — or meant
to, at the time.

| Location | What it means |
|---|---|
| `modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp:259` | `ambient_` assigned at `VoxiRenderer.cpp:247`, never read — **and so is `sunColor_`, which the sweep missed**. Investigating it corrected the claim: the ambient is *not* broken. `gAmbient` and `gSunColor` come from the device's frame constants via `setSkyAtmosphere` (`D3D12Device.cpp:2181`, `:2191`), and `SandboxApp.cpp:1136,1141` sets the same values there. `setSun`'s colour and ambient were pure redundancy — a second place the sun appeared configurable that changed nothing. **Fixed by narrowing the API to `setSunDirection`**, not by wiring a second source of truth. |
| ~~`sandbox/src/SandboxApp.cpp:5135` — `upgradeStatus_` takes four status messages and is never displayed~~ — **FIXED.** `setUpgradeStatus()` now also sets `upgradeStatusAge_`, and `SandboxApp` renders `upgradeStatus_` for 12 seconds after it is set (`SandboxApp.cpp` ~12370-12381), colour-coded on whether it starts with "Upgrade failed". The setter's own comment narrates the original bug in the past tense. |
| ~~`sandbox/src/SandboxApp.cpp:5093` — `worldSpace_` affects nothing but the button label~~ — **FIXED.** `gizmoBasis()` and the gizmo's draw matrix (`SandboxApp.cpp` ~10861, ~11192) both branch on `worldSpace_` now (with an explicit, commented exception: Scale always draws local, matching Unreal). The coordinate-space switch is live. |
| ~~`modules/audio/include/aver/audio/Mixer.hpp:167` — `lisFwd_` stored and never used, so listener orientation does not affect panning~~ — **FIXED.** `Mixer.cpp` (~line 324) now reads `lisFwd_` to compute a front/back dot product and applies a 3 dB cut when a source is behind the listener; the code comment there narrates the same fix. |
| ~~`sandbox/src/SkinSceneTest.hpp:90` — `ok_` set once, never read, so the test's pass/fail flag is ignored~~ — **FIXED.** `passed()` (which reads `ok_`) is now called from `SandboxApp::exitCode` (`SandboxApp.cpp:6527`) so `--skin-scene-test`'s exit code reflects it; the header's own comment narrates the same fix. |
| `sandbox/src/SkinSceneTest.hpp:86`, `:88` | `clipId_`, `offscreen_` — still assigned once in `SkinSceneTest.cpp` and never read anywhere. These two are the ones from this row still genuinely dead. |

Four of the six original findings above turned out to have been fixed since the sweep — each one
found by its own successor comment narrating the bug in the past tense, then confirmed by grep. Only
`clipId_`/`offscreen_` (and the already-resolved `ambient_`/`sunColor_` row, fixed a different way)
remain from this section.

## B. Documentation that actively misleads (8 confirmed + 7 of the 8 unverified)

Safe to fix, and the highest ratio of harm to effort — these send readers looking for things that do
not exist.

- ~~`shaders/README.md:3`~~ — described HLSL compiled by a tool called `aver-shaderc`. **FIXED by
  deleting the directory:** the tool never existed and HLSL lives in files under
  `modules/<mod>/shaders/` and `sandbox/shaders/`.
- ~~`tools/README.md:13`, `:38` — a tool table missing most tools, and a "Rust asset pipeline" of five
  tools that do not exist~~ — **FIXED.** The table now lists all eight real C++ tools under `tools/`
  (confirmed against `tests/formats/CMakeLists.txt`, `tests/editor/CMakeLists.txt` and the top-level
  `CMakeLists.txt`), the two Python scripts are named as a third kind, and the fictional Rust section
  is gone — there is no `.rs` file or `Cargo.toml` anywhere in this tree.
- ~~`interop/README.md:3`~~ — promised generated P/Invoke and Rust bindgen bindings. **FIXED by
  deleting the directory:** the C# bindings are hand-written under `scripting/csharp/`, and there is
  no Rust in this tree.
- ~~`editor/README.md:3`~~ — described a C#/.NET editor application. **FIXED by deleting the
  directory:** the editor is `sandbox/`.
- `docs/ABI.md:3`, `:5` — omits the `Aver.Audio.Abi` seam entirely; its own staleness callout is stale
- ~~`docs/SCENE_FRAMEWORK.md:7` — status banner says "only step 1 is built… no entity, no
  component"~~ — **FIXED.** The banner now says what actually shipped (a working scene layer and a
  working, differently-shaped class registry/spawn/tick in `Aver.Framework`) and what still has
  not (the editor's own viewport, which has not been migrated onto the ECS). §5's ABI listing also
  got a correction: the real `scene_abi.h` is the fully generic field surface, not the named
  per-property accessors the section sketches.

## C. The single biggest item — a whole dead seam — **FIXED**

`modules/audio.abi/include/aver/audio/audio_abi.h:28` — this used to say **all 24 exported entry
points** of `Aver.Audio.Abi` (`aver_audio_init` onward) had no caller, native, managed or test, and
that either wiring it or retiring it was the choice on the table. It has since been wired:
`sandbox/src/SandboxApp.cpp` now calls `aver_audio_init`/`aver_audio_shutdown` at startup/shutdown
(gated `AVER_SOUND_EDITOR_AUDIO`), and `scripting/csharp/Aver.Framework/Audio.cs` P/Invokes nearly
every remaining entry point for gameplay. The call site's own comment narrates the exact bug this
section described — *"NOTHING IN THE RUNTIME HAD EVER OPENED THE AUDIO DEVICE... The six nodes shipped
in 0.4.0 ... connect to an ABI whose device was shut"* — in the past tense, i.e. as a fixed defect.
`docs/ABI.md` still does not mention `Aver.Audio.Abi` exists, which is a live gap in that document
(not owned here).

## D. Dead functions — 28, and the judgement call

Mostly accessors: `posedEntities()`, `residentCount()`, `posedLastFrame()`, `entityCount()`,
`meshCount()`, `behaviourCount()`, `scenesBuilt()`, `textureCacheSize()`, `verticesPerSide()`.
These are the ones the API-surface contradiction applies to. **A separate, later sweep (commit
`b913718`, "Remove six dead accessors, and keep the seventh") settled the `residentCount()` case
specifically**: it exists on four unrelated classes (`FluidScene`, `SkinnedScene`, `SoftBodyScene`,
`ChunkStreamer`) where it is live API surface, and only `ThumbnailCache::residentCount()` (with its
neighbour `pendingCount()`) was genuinely unused — both have since been deleted. The other names in
this list have not been re-checked since.

A few are more clearly stale and worth a look regardless:

- `modules/formats/src/OcMat.cpp:322` — `saveOcmat()` has zero callers. **The original wording here
  said "the `.ocmat` writer is unreachable", and that was wrong.** `writeOcmat()` — the actual
  serialiser — is used and asserted three times in `MaterialTest.cpp` (`:167`, `:216`, `:333`). Only
  the ten-line wrapper that adds `create_directories` and an `ofstream` is uncalled, because
  `.ocmat` files are produced by `avermatc`, a **C#** tool. Left in place: deleting the file-writing
  half of a tested load/save pair to satisfy a metric is worse than leaving it, and any headless C++
  tool that ever writes a material wants exactly this function.
- `modules/platform/src/DirectoryWatcher.cpp:190` — `setDebounce()` implemented, never called
  (re-verified 2026-09-02, still the only hit for that name outside its own header)
- `modules/landscape/src/LandscapeRenderer.cpp:9` — `setSurface()` implemented, never called
  (re-verified 2026-09-02; its neighbour `setSurfaceBinding()` is a different function and does have
  a caller in `SandboxApp.cpp`, which is not the same thing)
- `modules/rhi.d3d12/src/D3D12Device.cpp:577` (moved from the originally-cited `:482` as the file
  grew) — file-local `isDepthFormat()` never called in its only TU, so this one cannot be public-API
  surface
- ~~`sandbox/src/AssetEditor.cpp:35` — `anyDirty()` has zero callers, so the unsaved-changes prompt
  it exists to drive never fires~~ — **FIXED.** `SandboxApp::requestExitChecked` now calls
  `assetEditors_.anyDirty()` and raises an "Unsaved changes" modal before exiting; the call site's own
  comment narrates this exact bug being closed. A single dirty editor's own tab-close is still silent
  (logs `AVER_WARN` and drops the edit) — only the whole-application exit path was wired.
- `modules/runtime.game/include/aver/game/GameLevel.hpp:55` — `pcgFields()`: the list is populated
  and never read as a list. (The singular `pcgField(name)` beside it *is* live — the sky uses it.)

---

## What the verifiers successfully defended (18)

Worth recording, because these are the traps a naive sweep falls into:

- **Exported C ABI for P/Invoke** — `aver_pbr_get/set_reflectance`, `aver_pbr_get/set_f90`,
  `aver_phys_set_fixed_step`, `aver_fw_scene_abi_version`. No in-tree caller by design; confirmed
  present in the shipped DLL export tables.
- **Exported members of a shared library** — `World::setLocalRotation/Scale` carry `AVER_SCENE_API`.
- **Shipped managed SDK surface** — `Vec3.DistanceSquared`, `Vec3.MoveTowards` in `Aver.Scene`.
- **"Duplicated" primitive builders** — `appendBox`/`appendSphere` in `GameMath.hpp`,
  `PreviewMeshCache.cpp`, and the four `xformPoint` definitions: each has callers in its own TU.
  Near-duplicate is not unused.
- **`Sample.Game`** — reached through `scripts/payload.allowlist:98`, not through a project reference.

---

## The 8 that were never checked

Do not treat these as clean. Seven are documentation claims; one is an orphan project.

`Aver.Scripting.Sample.csproj:1` · `docs/ABI.md:88` · ~~`docs/SCENE_FRAMEWORK.md:888`~~ ·
~~`modules/render/README.md:10`~~ · ~~`modules/render.gi/README.md:10`~~ ·
`content/legacy/README.md:3` · ~~`README.md:63`~~ · `modules/framework/README.md:55`

Four of the eight are now struck, checked afterwards. Two were resolved by deleting their
directories: both promised a "Phase 3" that had since been built elsewhere under other names, and
`docs/ARCHITECTURE.md`'s rows for `Aver.Render` / `Aver.Render.GI` already described that more
accurately. `content/legacy/README.md` was checked too and **survives** — it promises fixtures for
`.ocaero` and `.scene`, which `modules/formats/README.md:44` confirms are still unimplemented, so it
is a live placeholder. `README.md:63` was checked 2026-09-02 and is accurate — it correctly says
`interop/`, `editor/` and `shaders/` are gone and that `abi/` is a deliberate placeholder.
`docs/SCENE_FRAMEWORK.md:888` sat inside the §5.1 ABI sketch; checking it found the real
`scene_abi.h` is far leaner than that sketch (a fully generic field surface, no dedicated
`get_position`/`set_rotation`/etc.), and §5 now says so — see the note on `:7` above. The remaining
three (`Aver.Scripting.Sample.csproj:1`, `docs/ABI.md:88`, `modules/framework/README.md:55`) are
still genuinely unchecked.

Full data, including each verifier's reasoning: run the sweep again, or see the workflow journal at
`subagents/workflows/wf_2b54986f-7e5/journal.jsonl` while it survives.
