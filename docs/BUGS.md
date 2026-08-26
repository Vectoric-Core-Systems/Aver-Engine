# Verified bug list

Two sweeps, newest first. Each entry states the triggering input, because a defect nobody can
reproduce is a rumour.

---

# 0.5.0 — in progress, 2026-08-27

A six-lens sweep across rendering, physics/fluids, audio/animation, formats/save, graph/scripting and
the editor, each finding then given to a reproducer and to a separate pass told to refute it. Three
candidates were killed on that second pass — two as already-documented behaviour (`docs/ABI.md:1067`
states the character inner-body contact gap is deliberate; `docs/AVER_NODE_NODES.md:662` already
describes the SphereCast emitter gap) and one as failing the bar: a real observation about code that
produces no wrong output.

## Fixed

### 1. The audio device was never opened by the runtime — HIGH
`sandbox/src/SandboxApp.cpp`, fixed in `6d07e9d`

**Trigger: press Play on any level whose graph calls PlaySound, in a process where the Sound Editor's
preview button was never pressed.**

`aver_audio_init` had exactly one caller in the tree: that preview button. Every entry point in
`audio_abi.h` gates on the `g_started` flag only it sets, so `aver_audio_play` returned 0 and did
nothing. **0.4.0 shipped six Aver Node audio nodes and release notes saying the mixer finally had
callers; the nodes called an ABI whose device was shut.**

`aver_audio_collect`, which reclaims finished voices, had the same single caller — opening the device
without also wiring that would have traded silence for a fixed budget of 64 sounds per session, which
is worse because it works at first.

### 2. saveOcmap truncated its destination — HIGH
`modules/formats/src/OcMap.cpp`, fixed in `914f7fb`

**Trigger: save a level loaded from a legacy `.ocmap`, then crash or fill the disk mid-write.**

Written with `ios::trunc` **in the same commit (`e0ff3d3`) that converted its four siblings away from
it** — one change removing the pattern while another introduced a fresh instance. Each was internally
consistent; only the pair was wrong, which is why reviewing either alone would not have caught it.

### 3. AVR1's chunk-directory bound wrapped — HIGH
`modules/formats/src/Avr1.cpp:253`, fixed in `914f7fb`

**Trigger: open any AVR1 container whose `dirOffset` is near 2^64 with a small `chunkCount`.**

`dirOffset + chunkCount*40 > size` is unguarded 64-bit addition; it wraps to a small number, passes,
and the directory Reader starts at `bytes + dirOffset`. The sibling check twenty lines below was
already fixed and carries a comment explaining the mechanism — **the sweep that wrote it stopped one
check short inside the function it was correcting.** The header CRC is no defence; it is computed
over whatever the file says.

The regression test took three attempts, and that is the useful part: asserting only
`!parseAvr1(...)` passed against the bug (the parse still fails, later and by accident, when the
Reader walks off its own end), and the first crafted offset never actually wrapped. Only asserting
the *failure reason*, with an offset that genuinely overflows, tells the fix from its absence.

### 4. The PlayerStart marker outlived its level — HIGH
`sandbox/src/SandboxApp.cpp`, fixed in `a309a8f`

**Trigger: place a Player Start, then File > New Level, then Add > Player Start.**

The marker is deliberately transient and not in `levelEntities_`, so `unloadLevel`'s destroy loop
could not reach it. `loadLevel` reassigns it on the way in, which hid this for every path that ends
one level by opening another — leaving it visible only on the path that ends a level without opening
one. The stale marker kept rendering, and Add > Player Start selected it instead of creating a new
one, so the new level could not be given a spawn at all.

### 5. Low tier's shadow-tile amortisation — HIGH (reported as flicker)
`modules/render.voxi/src/Voxi.cpp`, fixed in `e9a7667` — see the 0.4.0 section's own lesson.

### 6. The viewport hint drew over any open drawer — MEDIUM
`sandbox/src/SandboxApp.cpp`, fixed in `55f5998`

**Trigger: open any drawer and look at its bottom ~70 px.**

The hint anchors to `vpY_ + vpH_`, and `vpH_` is not reduced when a drawer opens — a drawer is an
overlay, not a dock split. Cosmetic over the Content Browser; over the new Console it covered the
**input line**, and since the overlay is `NoInputs` the box could still be typed into blind.

## Open

- **The GI voxelisation gate ignores compute-skinned pose changes** — MEDIUM.
  `modules/render.voxi/src/VoxiRenderer.cpp:806-832`. `giDrawsKey()` hashes mesh handle, world
  transform and material but never the vertex buffer a compute skin pass writes, and `submit` applies
  no `meshVertexBuffer` filter. Once an animated character's world transform stops changing, the gate
  reports "unchanged" forever and its contribution to indirect light freezes at that pose.
  `PtSceneView::submitDraw` and Voxi's own BLAS cache both check `meshVertexBuffer` for exactly this
  reason; the GI gate is the one that does not.

  **Deliberately not fixed blind.** The obvious repair — treat any skinned draw as "changed" — forces
  revoxelisation every frame in any scene containing a character, which is precisely the ~10.5 ms
  saving `giUpdateInterval` was measured to buy. The right shape is probably to fall back to the
  interval schedule rather than skipping indefinitely, but that wants a measurement first.

---

# 0.4.0 — 2026-08-25

A release sweep at `b952b8a`, 146 commits after the 0.3.0 cut. Smaller than the 2026-08-03 sweep and
differently shaped: it went looking for what a person hits in the first five minutes, not for what a
static reading of the tree turns up.

## Fixed

### 1. The FirstPerson template rendered the inside of its own character — HIGH
`sandbox/src/SandboxApp.cpp`, `modules/scene/include/aver/scene/Components.hpp`

**Trigger: New Project → FirstPerson → Play.** That is the whole reproduction, and it is the first
thing a new user does.

The character gained a visible skinned body this release. Nothing told the renderer not to draw it
for the camera living inside its head, so the frame was a near-solid dark brown fill — the probe read
`(44,22,14)` — with a faint triangle-normal seam the only clue it was geometry at all. The log looked
healthy throughout: five classes declaring, a game mode beginning play with the right pawn and
controller, `11 drawn ... over 24 entities`.

Fixed with one flag bit, `kMeshRendererHiddenFromOwner`, authored as `hidden=owner` on a `COMP` line.
The mesh still goes through `submitShadowOnly`, so it casts its shadow, voxelises into GI, and stays
in the ray-tracing geometry table — only the raster colour draw is skipped. After: probe
`(203,214,216)`, sky; `1 owner-hidden` in the log; Voxi's shadow census still counting it among 16
draws. Third person is unaffected and needs no second branch.

### 2. Opening a legacy `.ocmap` and saving it deleted most of it — HIGH
`sandbox/src/SandboxApp.cpp`, `modules/formats/src/OcWorld.cpp`, `modules/formats/src/OcMap.cpp`

**Trigger: open any level using `ROOT`, `CLIENT`, `SURFACE`, `GROUND`, `KILLZ` or `DEFORM`, then
save it.**

`loadLevel` called `loadOcworld` for every level. `parseOcworld` accepts an `OCMAP` header as readily
as an `OCWORLD` one, so it never failed — but `OcWorldData` has nowhere to put those six record
kinds and no `DEFORM` branch at all. They were dropped on load, and `saveLevel` wrote back only what
survived. Success was reported at every step.

**The fix that was nearly shipped would have been worse than the bug.** Dispatching on the header
token is the obvious approach and it is wrong: `AverProjects/ElectricDreams/Content/Maps/Default.ocmap`
and `AverProjects/FirstPerson/Content/Maps/Default.ocmap` both begin with the literal line `OCMAP 1`
while their content is pure OCWORLD grammar. A header check would have routed both into the legacy
parser and destroyed their sky, fog, terrain and PCG volumes on first save. `levelFileIsLegacyOcmap`
scans for the six legacy-exclusive records instead, and classifies all five real level files in the
tree correctly.

### 3. Four savers truncated the destination before knowing the new content was good — MEDIUM
`modules/formats/src/OcGraph.cpp`, `OcMat.cpp`, `OcWorld.cpp`, `sandbox/src/GraphEditor.cpp`

**Trigger: lose power, crash, or fill the disk during a save.** Rare per save; not rare across a
project's life.

Each opened `std::ofstream(path, ios::trunc)` directly, so the file was destroyed at `open()`.
`OcSave.cpp` and `Settings.cpp` already shipped the temp-then-rename dance; it is now
`writeFileBytesAtomic` / `writeFileTextAtomic` in the platform layer and all four go through it.
`writeFileBytes`/`writeFileText` keep their old contract on purpose — `OcBt.cpp` and `OcNav.cpp` call
them directly and their own comments accept the truncate risk for regenerable build output.

Proved by falsification: reverting the implementation to the truncating one fails exactly the three
new assertions that check the original file survives, and no others.

### 4. Raising path-tracing quality on a live view removed the GPU — HIGH
`modules/render.pt/src/PtSceneView.cpp`, `modules/rhi.d3d12/src/D3D12Device.cpp`

**Trigger: with path tracing already running, raise its quality tier — Low to Epic reaches it most
reliably.**

Resizing the accumulator rewrote the denoiser's descriptors to the new size over buffers allocated at
the old one. The shader walked off the end and the device was lost with `DXGI_ERROR_DEVICE_HUNG`.
It read as intermittent because whether an overrun faults depends on what is allocated past the end,
and it read as setting-specific because the overrun scales with the size of the jump.

Allocate-once-plus-describe-every-time is the pattern; look for it wherever a resource is guarded by
`if (!handle)` and its view is written unconditionally below. **D3D12 does not reject an oversized
view at creation without the debug layer**, so nothing between the mistake and the dead GPU said a
word. The RHI now refuses one outright. `--pt-quality-ramp` exercises it.

### 5. A lost device was fed a fresh command list every frame — MEDIUM
`modules/runtime/src/Engine.cpp`, `modules/rhi.d3d12/src/D3D12Device.cpp`

`waitFence` detected device removal and **both callers discarded the bool**. `present` then advanced
a fence value the device could never signal. The result was the engine "crashing with no message".
`IDevice::deviceLost()` now exists, the reason is decoded and logged once, the window title says so,
and the frame loop stops drawing. It does not exit — nothing can recreate the device, and exiting
would take the explanation off the screen. `--device-lost-at <N>` stages it.

## Open at 0.4.0

These ship. Each is listed in the release notes; the triggering input is here.

- **A fluid volume pancakes.** Trigger: simulate any fluid, including at the proportions its own unit
  calibration produces.
- **Vulkan never writes the shadow cascade map.** `pushRenderScope` discards the caller's render
  targets. Trigger: build with `AVER_RHI_VULKAN=ON` and look for a cascade shadow. Masked by RT
  shadows. Off by default.
- **GI can be dominated by one saturated emitter.** The gather clamp (`AVER_VOX_MAXRAD`, 16.0) is
  per component, so it bounds magnitude and not hue. Trigger: three 1.8-metre pure-red spheres under
  a 100,000-lux sun. Workaround: scale them down.
- **Saving a level, or an animation, reloads it** over the editor's undo history, selection and
  camera. Trigger: save.
- **Pre-SM6 hardware or `--force-caps no-dxc` silently drops UI, skinning, fluids, particles and
  occlusion culling**, with no message.
- **The occlusion culler retries its failed shader compile every frame.**
- **The Voxi voxel-grid resolution control does nothing after startup.**
- **A legacy `.ocmap` `DEFORM` placement is shown as a small static box**, there being no
  deformable-cage system to draw it. It round-trips faithfully.
- **`modules/runtime.game` is a diverged twin** of the sandbox's camera and scene walk. Nothing
  instantiates it, so it received neither this release's owner-hide fix nor the earlier
  culling-starves-render-features fix. Dead code that looks live.

## The lesson from this sweep

The 2026-08-03 sweep closed with *"nothing here was caught by the gates"*. This one is worse, and it
is about the gates themselves.

On 2026-08-18, `3302f89` made ray-traced sun shadows the engine default. The gate definitions were
last edited 2026-08-02 and the baselines recorded 2026-08-11 — both before the flip. From that
commit, **every gate that did not name an RT flag stopped measuring the raster path** and became a
duplicate of its `-rt` twin. `centre` duplicated `rt`, `shadow` duplicated `shadow-rt`, `penumbra`
duplicated `penumbra-rt`. Nine of eighteen gates went blind and the oracle kept reporting
confidently.

The eighteen value mismatches on their own looked exactly like ordinary shading drift across 146
commits, and re-recording would have frozen the collapse in permanently. The **only** thing that
caught it was the `penumbra` invariant, which exists solely to assert that two gates measuring
different code paths still disagree — and which `-Record` counts as a failure precisely so that a
broken invariant cannot be recorded away.

This is the second time a default flip has silently eaten the oracle; the same thing happened when
GI became the default, and the fix then was the same `--no-gi` flags now sitting beside `--no-rt`.

**A gate that names no flag measures whatever the default happens to be, which is not a fixed
quantity.** The invariant checks are not decoration around the probe values — on this evidence they
are the more valuable half of the oracle.

---

# 2026-08-03

Six independent lenses swept the tree at `f0d6ca4` and produced **36 candidates**. Every one was then
given to a separate verifier prompted to *refute* it — to default to "this claim is wrong" and go
looking for the guard, the caller, or the misread convention that would kill it.

| | |
|---|---|
| Candidates | 36 |
| Verified | **36** (none unverified) |
| Refuted and dropped | 12 |
| **Confirmed** | **24** — 8 high, 14 medium, 2 low |

Unlike the stale-code sweep, there is no ambiguity band here: these are not "unused" findings where
"is it dead?" is a judgement call. Each confirmed entry is code that runs and does the wrong thing,
with the triggering input stated.

**ALL 24 ARE NOW FIXED**, plus the viewport-settling flake found while cutting the release. Nothing
here was speculative — the twelve that could not be positively confirmed were dropped, not
downgraded.

Each fix is described at its entry. Where a fix changed an interface or a state machine rather than
one expression, the reasoning is in the code at the site, not only here.

---

## Fixed

### 1. `eulerDegFromQuat`'s gimbal branch corrupted saved levels — HIGH
`sandbox/src/SandboxApp.cpp:178` → now `sandbox/src/EditorEuler.hpp`

The gimbal-lock branch divided by `1 - 2(y² + z²)`, which for this ZYX composition is
cos(pitch)·cos(yaw) — **identically zero exactly when the branch fires**. `atan2` with a ~0
denominator returns whatever the float residue's sign says.

Measured before the fix, `|dot|` of the recomposed quaternion against the original:

| input (roll, pitch, yaw) | read back | \|dot\| |
|---|---|---|
| (0, 90, 30) | (0, 90, 90) | 0.866 — 60° wrong |
| (0, 90, 0) | (0, 90, −180) | **0.000 — a full 180° flip** |
| (0, −90, 45) | (0, −90, 90) | 0.924 |

The band is `|sin(pitch)| > 0.99999` — **0.26° around vertical**, reachable by typing 90 into the
inspector or dragging a rotation ring through vertical. And it was not merely a display glitch:
the level save reads rotations back through this function, so any near-vertical entity was
**persisted wrong to the `.ocworld`**. Save and reload silently reoriented it.

Fixed by dividing by `1 - 2(x² + z²)` = cos(yaw − roll), which pairs with the existing numerator.
The pair was extracted to `EditorEuler.hpp` so it could be tested at all — it had been two `static`
functions unreachable from any test. `EditorEulerTest` now sweeps **3601 pitches from −90 to +90**;
worst `|dot|` is 1.000000.

### 2. The game runtime rendered every frame with the previous frame's camera — HIGH
`modules/runtime.game/src/GameApp.cpp:580`

`Engine::frameStep` runs `onUpdate → beginFrame → onRender → endFrame`, and `beginFrame` takes the
**one and only** snapshot of `PerFrameCB` into the GPU-visible buffer. `GameApp` called `pushFrame`
— which is what calls `setCamera` and `setSkyAtmosphere` — as the first line of `onRender`, *after*
that snapshot. So every pixel of frame N was rasterised with frame N−1's view-projection, camera
position, sky, fog and cloud constants.

Worse than a uniform one-frame lag: `viewProj_` is also what `drawWorld` culls against, so culling
used **this** frame's matrix while the GPU drew with the **previous** one — the two disagreed by
exactly one frame of camera motion, so fast panning could cull geometry that was on screen.

The editor never had it: `SandboxApp` sets its camera in `onUpdate`. That is why no gate caught it.
Fixed by moving `pushFrame` to the end of `onUpdate`.

### 3. AVR1 chunk bounds check wrapped — HIGH
`modules/formats/src/Avr1.cpp:263`

`if (off + onDisk > size)` with both operands `u64` read straight from the file. `off = 2^64−1`,
`onDisk = 2` sums to `1`, passes, and the following `c.data.assign(bytes + off, ...)` reads from
`bytes + 2^64−1`. Rearranged to `off > size || onDisk > size - off`, where neither side can overflow.

`.ocmesh`, `.ocanim`, `.ocskel`, `.ocland` and `.octex` are all AVR1 containers, so this was the
front door for every binary asset the engine loads.

### 4. glTF indices were never bounds-checked — HIGH
`modules/formats/src/GltfImport.cpp:322`

The flat-normal generator indexed `m.positions` and `m.normals` directly with index values from the
file. A glTF naming vertex 40000 in a 12-vertex primitive wrote three floats well outside both
vectors — heap corruption reachable by opening a downloaded model. Every index is now validated
before any is used, and an out-of-range one **refuses the import** rather than being clamped: an
index past the end is not one bad triangle, it is an index buffer that does not describe its vertex
buffer.

---

## The other twenty, and what each fix did

### HIGH

- **`Aver.Framework/Character.cs:175`** — view pitch was applied with the wrong sign. `Rot.ToQuat`
  builds pitch about `Vec3.Right = (0,1,0)`, and rotating forward `(1,0,0)` about +Y by a *positive*
  angle sends it to `(cos, 0, −sin)` — toward −Z, which is **down**. But `LookDirection` returns
  `z = sin(pitch)` (up), `DriveWithInput` passes `−MouseDeltaY` to match, and `PitchMin/PitchMax` are
  written the same way. So mouse-look was inverted *and* `LookDirection` — what aiming and traces use
  — disagreed with where the camera pointed. Two wrongs that did not cancel. **Negated at the one
  site that was the odd one out.**
- **`Mixer.cpp:181` / `:196`** — voice stealing raced the audio thread. The state stayed `Active` for
  the whole of `renderVoice`, so `play()`'s `Active → Pending` CAS succeeded *mid-render*, dropped the
  sound's refcount — freeing samples being read — and overwrote `vo.sound` and `vo.cursor` underneath
  it. **Fixed with a fourth state:** the audio thread CASes `Active → Rendering` for the duration, so
  the stealing CAS fails and moves on. Stealing now retries a few times, because a failed CAS no
  longer means "taken" but may mean "being rendered", and every "is it playing" query accepts both
  states or `playing()` would flicker with the audio callback.
- **`HostBridge.cs:745`** — `DispTickAll` indexed the live bucket while `DispUnbind` removes from it,
  so an actor destroying an actor shifted the list and `++i` stepped over whoever slid into the gap.
  **Now walks a snapshot**, skipping actors that left during the walk — identified by reference, not
  just by entity id, since an id can be reused by a spawn in the same tick.

### MEDIUM

- **`RHIShaders.cpp:420`** (and the Voxi and actor-preview vertex shaders) — normals went through the
  world matrix instead of its inverse transpose. **Fixed with one `averTransformNormal` in the shared
  prelude**, using the cofactor matrix: `cofactor(M) == det(M)·inverse(M)ᵀ`, and since the caller
  normalises, the determinant divides out — no inverse, no division, no singular case. Moved **zero**
  gate probes, as predicted: the gate scene's only non-uniform object is an axis-aligned box, whose
  normals lie along principal axes and survive a diagonal scale.
- **`OcMesh.cpp:405`, `OcAnim.cpp:310`, `GltfImport.cpp:112` (+ the accessor offset beside it),
  `Avr1.cpp:140`** — the `a + b > limit` overflow family, all rearranged to `a > limit || b > limit − a`.
  glTF additionally **rejects negative** `byteOffset`/`byteLength` before the cast to `u64`, which is
  how they became near-2⁶⁴ in the first place. `OcMesh` gained the check it never had: the UV, joint
  and weight *offsets* come from the file and were added to a cursor bounded only by the *strides*.
- **`PcgVolume.cpp:236`** — read back one frame after recording the copy, but the device is double
  buffered, so at frame N+1 only frame N−1 is known complete. **Waits three frames now**, with the
  constant named and documented rather than left as a magic 1.
- **`VoxiRenderer.cpp:637`** — the ray-tracing instance table was one upload buffer rewritten every
  frame while the GPU read the previous frame's copy from the same memory. **Now a 3-deep ring**,
  rotated before writing.
- **`D3D12Device.cpp:1953`** — `createSkinTargetMesh` left `ibBuffer` and `vertexCount` at 0, so
  `meshGeometry()` refused every skin target and one skinned entity switched ray-traced reflections
  off scene-wide. **Both now carried across from the source mesh.**
- **`ChunkMesh.cpp:49`** — the central-difference normal clamped the low side but not the high, and
  `heightAt` returns 0 out of range while heights are *absolute world Z*. On the last row and column
  the gradient was computed against sea level, so terrain 300 m up got near-sideways normals along
  every section edge. **Both sides clamped.**
- **`Win32DirectoryWatcher.cpp:148`** — the worker exited on three failure paths without recording
  anything, and `watching()` tests only that the backend object exists. Hot reload stopped silently
  for the rest of the session. **The backend now reports `died()` and `watching()` honours it.**
- **`McpBridge.cpp:528`** — every `abi` command ran its dispatcher twice, once in `pump()` and again
  in the host's apply callback, while the reply described only the first. **No longer forwarded.**
- **`McpBridge.cpp:482`** — `stop()` closed the listener but not the accepted connection, which was a
  local in the worker, so a worker inside `recv()` never woke and `join()` waited for the client to
  disconnect. **The client socket is published and closed too**, with an atomic exchange so it cannot
  be closed twice.
- **`AverDesign.cpp:96`** — the Roslyn backend lacked the plain `Controller` suffix rule the built-in
  scanner has, directly under a comment promising the two classify identically. `FpsController` was a
  PlayerController to one and Unknown to the other. **Added, below `PlayerController` so the longer
  suffix still wins.**
- **`SampleHud.cs:33`** — the reference HUD used the viewport's size but not its origin, so it drew
  relative to the window: in the editor the health bar sat under the Content Browser and the
  crosshair missed the middle of the image. **Every coordinate now offset by `vp.X`/`vp.Y`** — and
  this is the file every project copies its HUD from.

### LOW

- **`AudioDeviceWasapi.cpp:62`** — on the 5 s start-up timeout the main thread closed `readyEvent_`
  while the render thread might still `SetEvent` it. **The thread is joined before the handle is
  closed**, which only matters when start-up is already slow — exactly when it is least survivable.

---

## Found while cutting the release, not by the sweep

### The editor's viewport is not settled when a capture run samples it — MEDIUM

Observed directly, twice, on the **same binary**:

```
run 1:  ms-gi-debug FAIL [BAD-PROBE tiny-rect 0,270 45x24]   shadow FAIL [BAD-PROBE ...]
run 2:  ms FAIL, rt FAIL, gi FAIL   /   ms-gi-debug PASS, shadow PASS
        ms-rt  FLAKY  first=rect 60,132 96x24  /  retry=rect 0,270 2750x1639
```

A different set of gates fails each time, and the FLAKY lines print the two rects side by side: the
probe sometimes lands while the ImGui dockspace still reports a 45×24 or 96×24 central node instead
of 2750×1639. The pixel returned is the window background, which is why every affected gate returns
the identical `14,14,16`.

`gates.ps1` already detects this (`BAD-PROBE`) and retries, so the oracle is not lying — but it means
a capture run's frame budget is not reliably enough for the layout to settle, and any tool comparing
raw probe codes between two runs will see phantom differences. `verify-payload.ps1` did exactly that
and blocked a release over it.

**The fix belongs in the editor, not in the tooling**: a capture run should not sample until the
dockspace has produced a plausible viewport, or `--frames` should not begin counting until it has.
Skipping disowned probes, which is what the tooling does now, treats the symptom.

## Themes worth acting on rather than patching case by case

**Parser bounds arithmetic overflows.** Five of the confirmed bugs are the same mistake in five
files: `a + b > limit` where `a` and `b` are unsigned values read from a file. Two are fixed; the
pattern is `a > limit || b > limit - a`. Every binary reader in `modules/formats` should be swept
for it at once rather than one bug report at a time.

**The audio mixer's voice-stealing path is not thread-safe.** Two confirmed use-after-free style
races (`Mixer.cpp:181` and `:196`) both come from the control thread mutating voice state the audio
callback is reading. This needs a design fix — a lock-free handoff or a retire queue — not two spot
patches.

**Nothing here was caught by the gates**, and that is the honest lesson. The gate oracle samples
probe pixels in the editor; it cannot see a game-runtime frame ordering bug, a C# character
controller, an audio race, or a malformed-asset code path. Bit-exact pixel gates are a strong guard
over exactly the surface they cover, and the surface is narrower than it feels.
