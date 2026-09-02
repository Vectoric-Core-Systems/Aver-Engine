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

## A. Real defects — a variable is written and never read (7)

These cannot be defended as API surface. Each one means a feature silently does nothing.

| Location | What it means |
|---|---|
| `modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp:259` | `ambient_` assigned at `VoxiRenderer.cpp:247`, never read — **and so is `sunColor_`, which the sweep missed**. Investigating it corrected the claim: the ambient is *not* broken. `gAmbient` and `gSunColor` come from the device's frame constants via `setSkyAtmosphere` (`D3D12Device.cpp:2181`, `:2191`), and `SandboxApp.cpp:1136,1141` sets the same values there. `setSun`'s colour and ambient were pure redundancy — a second place the sun appeared configurable that changed nothing. **Fixed by narrowing the API to `setSunDirection`**, not by wiring a second source of truth. |
| `sandbox/src/SandboxApp.cpp:5135` | `upgradeStatus_` takes four status messages ("Project upgraded…", "Upgrade failed: …") and is **never displayed**. The project-upgrade flow reports nothing to the user. |
| `sandbox/src/SandboxApp.cpp:5093` | `worldSpace_` affects nothing but the label of the button that toggles it. The gizmo coordinate-space switch is inert. |
| `modules/audio/include/aver/audio/Mixer.hpp:167` | `lisFwd_` — the listener's forward vector is stored per set and never used, so **listener orientation does not affect panning**. |
| `sandbox/src/SkinSceneTest.hpp:90` | `ok_` is set true once and never read — **the test's own pass/fail flag is ignored**. |
| `sandbox/src/SkinSceneTest.hpp:79`, `:81` | `clipId_`, `offscreen_` assigned once, never read. |

Verified independently for `ambient_` and `upgradeStatus_`: both confirmed by direct grep.

## B. Documentation that actively misleads (8 confirmed + 7 of the 8 unverified)

Safe to fix, and the highest ratio of harm to effort — these send readers looking for things that do
not exist.

- ~~`shaders/README.md:3`~~ — described HLSL compiled by a tool called `aver-shaderc`. **FIXED by
  deleting the directory:** the tool never existed and HLSL lives in files under
  `modules/<mod>/shaders/` and `sandbox/shaders/`.
- `tools/README.md:13`, `:38` — a tool table missing most tools, and a "Rust asset pipeline" of five
  tools that do not exist. **Still open** — `tools/` holds real source, so it is a README to correct
  rather than a directory to remove.
- ~~`interop/README.md:3`~~ — promised generated P/Invoke and Rust bindgen bindings. **FIXED by
  deleting the directory:** the C# bindings are hand-written under `scripting/csharp/`, and there is
  no Rust in this tree.
- ~~`editor/README.md:3`~~ — described a C#/.NET editor application. **FIXED by deleting the
  directory:** the editor is `sandbox/`.
- `docs/ABI.md:3`, `:5` — omits the `Aver.Audio.Abi` seam entirely; its own staleness callout is stale
- `docs/SCENE_FRAMEWORK.md:7` — status banner says "only step 1 is built… no entity, no component"

## C. The single biggest item — a whole dead seam

`modules/audio.abi/include/aver/audio/audio_abi.h:28` — **all 24 exported entry points** of
`Aver.Audio.Abi` (`aver_audio_init` onward) have no caller, native, managed or test. Either wire it
or retire it; it is currently cost with no consumer. `docs/ABI.md` does not mention it exists.

## D. Dead functions — 28, and the judgement call

Mostly accessors: `posedEntities()`, `residentCount()`, `posedLastFrame()`, `entityCount()`,
`meshCount()`, `behaviourCount()`, `scenesBuilt()`, `textureCacheSize()`, `verticesPerSide()`.
These are the ones the API-surface contradiction applies to.

A few are more clearly stale and worth a look regardless:

- `modules/formats/src/OcMat.cpp:322` — `saveOcmat()` has zero callers. **The original wording here
  said "the `.ocmat` writer is unreachable", and that was wrong.** `writeOcmat()` — the actual
  serialiser — is used and asserted three times in `MaterialTest.cpp` (`:167`, `:216`, `:333`). Only
  the ten-line wrapper that adds `create_directories` and an `ofstream` is uncalled, because
  `.ocmat` files are produced by `avermatc`, a **C#** tool. Left in place: deleting the file-writing
  half of a tested load/save pair to satisfy a metric is worse than leaving it, and any headless C++
  tool that ever writes a material wants exactly this function.
- `modules/platform/src/DirectoryWatcher.cpp:190` — `setDebounce()` implemented, never called
- `modules/landscape/src/LandscapeRenderer.cpp:9` — `setSurface()` implemented, never called
- `modules/rhi.d3d12/src/D3D12Device.cpp:482` — file-local `isDepthFormat()` never called in its
  only TU, so this one cannot be public-API surface
- `sandbox/src/AssetEditor.cpp:35` — `anyDirty()` has zero callers, so **the unsaved-changes prompt
  it exists to drive never fires**
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

`Aver.Scripting.Sample.csproj:1` · `docs/ABI.md:88` · `docs/SCENE_FRAMEWORK.md:888` ·
~~`modules/render/README.md:10`~~ · ~~`modules/render.gi/README.md:10`~~ ·
`content/legacy/README.md:3` · `README.md:63` · `modules/framework/README.md:55`

The two struck rows were checked afterwards and resolved by deleting their directories: both promised
a "Phase 3" that had since been built elsewhere under other names, and `docs/ARCHITECTURE.md`'s rows
for `Aver.Render` / `Aver.Render.GI` already described that more accurately. `content/legacy/README.md`
was checked too and **survives** — it promises fixtures for `.ocaero` and `.scene`, which
`modules/formats/README.md:44` confirms are still unimplemented, so it is a live placeholder.

Full data, including each verifier's reasoning: run the sweep again, or see the workflow journal at
`subagents/workflows/wf_2b54986f-7e5/journal.jsonl` while it survives.
