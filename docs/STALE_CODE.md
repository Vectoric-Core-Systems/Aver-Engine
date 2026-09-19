# Stale code sweep — 2026-08-03

> **THE `sandbox/` CITATIONS BELOW ARE ALL DEAD — re-resolve before acting on one (added 2026-09-20).**
> This sweep is pinned to `8fcfd71` and predates the 2026-09-16 split of a 29,952-line
> `SandboxApp.cpp` across ~40 files (`sandbox/CMakeLists.txt:9-11`). That file is now **2,968 lines**,
> so every `SandboxApp.cpp:5135` / `:5093` / `~12370` / `~10861` / `~11192` / `:6527` in the FIXED
> narrations below points past its end. The *findings* those narrations describe were checked and are
> not re-opened by the move — only the coordinates rotted. The surviving work list has been
> re-verified against HEAD (`ba9c94aa`, 1,176 commits) and restamped in place; where a bullet's line
> number moved, the new one is given.
>
> **Do not read this document as current.** A sweep is a photograph. This one is six weeks and 659
> commits old, and re-running it would find a different set.

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
- ~~`docs/ABI.md:3`, `:5` — omits the `Aver.Audio.Abi` seam entirely~~ — **PARTLY FIXED 2026-09-20,
  and the bullet understated it.** Two seams were missing, not one: `Aver.Audio.Abi` (25
  `AVER_AUDIO_API` entry points) and `Aver.Settings` (14 `AVER_SETTINGS_API` entry points, SHARED at
  `modules/settings/CMakeLists.txt:7`), against which `docs/ABI_VERIFICATION_PLAN.md:348` had counted
  nine all along. `ABI.md`'s opening now says nine, its §2 table carries a row for each, and a new
  §19 describes both. **Still open:** neither has a section walking it entry point by entry point the
  way §§3–10 do the other seven, so the headers remain the reference for their surfaces.
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
**Three corrections, 2026-09-20.** The header now declares **25** entry points, not 24 — one was
added after this section was written and the count was never bumped. `docs/ABI.md` no longer fails to
mention the seam: §2's table has a row for it and the new §19 describes it, so the "live gap in that
document" this paragraph used to end on is closed. And the editor's call site moved out of
`SandboxApp.cpp` in the 2026-09-16 file split — the device is opened at
`sandbox/src/SoundEditor.cpp:510` today — while `AverEngineRuntime.exe` now links the seam too
(`Runtime/CMakeLists.txt:153-154`), which it did not when this was written.

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

- ~~`modules/formats/src/OcMat.cpp:322` — `saveOcmat()` has zero callers~~ — **RESOLVED, and this
  bullet was wrong twice over (struck 2026-09-20).** It now has **two** callers:
  `modules/formats/src/MaterialCook.cpp:209` and `tools/AverAssetC.cpp:508` — precisely the "headless
  C++ tool that ever writes a material" the old wording said would want it. `MaterialCook.hpp:16`
  says so in prose as well ("writeOcmat/saveOcmat already exist and are covered by…"). The definition
  has also moved to `OcMat.cpp:483`, 161 lines from the cited `:322`. Nothing here needs doing; the
  bullet is kept struck rather than deleted so the next reader inherits the answer.
- `modules/platform/src/DirectoryWatcher.cpp:195` — `setDebounce()` implemented, never called
  (still live: re-verified 2026-09-20, and the only hits for that name are its definition and its
  declaration at `DirectoryWatcher.hpp:48`. The line moved from `:190`.)
- `modules/landscape/src/LandscapeRenderer.cpp:9` — `setSurface()` implemented, never called
  (re-verified 2026-09-02; its neighbour `setSurfaceBinding()` is a different function and does have
  a caller in `SandboxApp.cpp`, which is not the same thing)
- `modules/rhi.d3d12/src/D3D12Device.cpp:598` (`:482` originally, then `:577`; the file keeps
  growing) — file-local `isDepthFormat()` never called in its only TU, so this one cannot be
  public-API surface. Still live: re-verified 2026-09-20.
- ~~`sandbox/src/AssetEditor.cpp:35` — `anyDirty()` has zero callers, so the unsaved-changes prompt
  it exists to drive never fires~~ — **FIXED.** `SandboxApp::requestExitChecked` now calls
  `assetEditors_.anyDirty()` and raises an "Unsaved changes" modal before exiting; the call site's own
  comment narrates this exact bug being closed. A single dirty editor's own tab-close is still silent
  (logs `AVER_WARN` and drops the edit) — only the whole-application exit path was wired.
- `Runtime/include/aver/game/GameLevel.hpp:106` — `pcgFields()`: the list is populated and never read
  as a list. (The singular `pcgField(name)` beside it *is* live, at `Runtime/src/GameLevel.cpp:356` —
  the sky uses it.) Still live: re-verified 2026-09-20; the declaration moved from `:55`.

---

## E. Added 2026-09-20 — checked at `ba9c94aa`, not by the 8fcfd71 sweep

Four items found by a later read-only sweep and **individually re-verified against HEAD before being
written down here**. Each is a grep anyone can repeat; none of them is a "looks unused" hunch.

- **`modules/core/include/aver/core/ModuleCheck.hpp` is included by nothing, and its macro is invoked
  nowhere.** A grep for `ModuleCheck` across the whole tree (excluding `build*/` and
  `.claude/worktrees/`) hits only `docs/ABI_VERIFICATION_PLAN.md`; a grep for `AVER_REQUIRE_MODULE`
  hits only its own definition at `ModuleCheck.hpp:33-36` and that same document. No module public
  header includes it. So the module-DAG guard it describes catches nothing at all — which is the
  exact *"looks thorough, checks nothing"* shape `ModuleCheck.hpp:10-12` warns about, turned on
  itself. **Either invoke it from the public headers of the modules that have dependencies, or delete
  the header and correct the doc.** `AVER_MODULE_ON(m)` (`:46`) goes the same way: one occurrence in
  the tree, its own definition, so the defined-vs-undefined distinction it exists to draw is drawn
  nowhere.
  **2026-09-20: the header now says this about itself**, in place of the paragraph that claimed each
  dependent module already invokes the macro. The mechanism is KEPT, not deleted — it is correct, and
  `ABI_VERIFICATION_PLAN.md` §3 reasons from it — so the item stays open as *adoption*: one
  `AVER_REQUIRE_MODULE` line per module with `DEPS`, provable only by building the combinations
  `scripts/module-matrix.ps1` builds, which is why it was not landed alongside the comment.
- **`docs/ABI_VERIFICATION_PLAN.md:109` states the opposite as settled fact** — "The existing fix is
  that each dependent module invokes `AVER_REQUIRE_MODULE` from its **own** public header." No module
  does. Corrected in place on 2026-09-20; recorded here because it is the same defect class as
  section B, a document asserting a closed item that was never opened.
- **`modules/rhi/include/aver/rhi/RHIResources.hpp:399` — `declaredUavCount` has zero callers**, while
  its immediate sibling `declaredSrvCount` has eighteen hits across both backends and the shared
  HLSL. The asymmetry is structural rather than accidental: the derived-register trick (the instance
  SRV at `t(declaredSrvCount)`) exists only on the SRV side, so there is no UAV analogue to feed.
  Safe to delete — or keep it and say in one line that it is there for symmetry, because as it stands
  it invites a reader to assume a UAV register-derivation convention that does not exist.
  **2026-09-20: deleted**, and `declaredSrvCount`'s own comment now states why it has no twin, so the
  symmetry argument cannot quietly put one back.
- **`modules/core/include/aver/core/ErrorCodes.hpp:82` — the `AbiError`-taking `abiErrorName` has no
  callers**; every call site uses `abiErrorNameOf(i32)` instead (`ErrorCodes.cpp:20`, used three times
  in `tests/abi/src/AbiEnumTest.cpp:976-979`). Low priority on its own, but this is an ABI-adjacent
  header where two similarly-named functions over two numeric vocabularies sit side by side, so
  picking the wrong one is a live hazard rather than a style question. Either delete it or say in one
  line that it is the enum-typed convenience for a caller that already holds an `AbiError`.
  **2026-09-20: deleted.** It also carried a second copy of the same eight strings — a table to
  forget to extend the next time a code is appended, across an ABI seam whose own rule is "append,
  never insert". `abiErrorNameOf(i32)` is now the only namer; a caller holding an `AbiError` casts.

**Two findings from the same sweep are deliberately NOT listed here, because they were fixed while it
ran.** `frameworkKeyFromMouseButton` (`InputKeys.hpp:63`) was reported as having no callers; it is
called from `Runtime/src/GameInput.cpp:97`, and the comment at `:90` explains the change. `deliver()`
/ `VoxiDelivery` (`SceneSubmission.hpp:216, 245`) was reported as having no production callers; it is
called from `Runtime/src/GameRender.cpp:579`. Recorded so nobody re-opens them from a stale list.
(`sandbox/src/SandboxRender.cpp:1137` still only *mentions* `deliver()` in a comment — whether the
editor's own walk should call it too is an open question, not a finding.)

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
