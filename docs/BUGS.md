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

### 7. Every `RENDER.*` command-line override was silently discarded by the project manifest — HIGH
`sandbox/src/SandboxApp.cpp` (`applyProjectRenderSettings`), fixed in `31c06a4` — this entry said "not
yet committed at time of writing" and that commit has since landed (2026-08-29).

**Trigger: pass any `RENDER.*` CLI override — e.g. `--rt-render-mode 0` — against a project manifest
that sets the same key.** The flag is accepted, parsed, and then thrown away with no diagnostic.

CLI overrides (`rtRenderModeOverride_` and eight siblings covering GI quality, ray tracing, path
tracing, voxel resolution, GI intensity/max distance, shadow rays, pixels-per-ray-tile and shadow
denoise) are applied exactly **once, at startup**, before any project is open.
`applyProjectRenderSettings` runs **later** — every time a project opens — and every line in it
unconditionally copied the manifest's `RENDER.*` value over whatever the flags had just set. It ran
after startup by construction, so it always won, and it logged nothing, so nobody watching the
console had any reason to suspect the flag they typed had been overwritten minutes earlier.

**It does not corrupt an image, it corrupts measurements — that is what makes it worse than an
ordinary rendering bug.** A whole raster-versus-ray-driven investigation set `--rt-render-mode 0` for
its raster half against a project manifest pinning `RENDER.RTRENDERMODE 1`, got mode 1 both times,
and reported the two paths as **"pixel-identical" and "at performance parity."** Both conclusions
were confidently published and both were wrong for the same reason: there was only ever one render
path in that comparison. It cost a real all-white shading bug (the ray-driven path was building its
surface from `mat.baseColorFactor` alone instead of multiplying it against the per-draw value, the
way the raster shader does) being argued away as noise, for as long as the flag bug stood between the
person testing and the renderer they thought they were testing.

**The "~3x per-frame cost gap" this entry originally also claimed has itself been retracted.** Fixing
the flag produced a *second* wrong number rather than a right one, because two further defects stood
behind it — see "Three features, one scene, and nobody said who won" below. The measured figures, once
all three were fixed, are the opposite of the reported ones: ray-driven is **1.9x faster** than the
rasteriser on that range, not 3x slower.

**This is the second time this exact defect has been fixed, not the first.** The first instance was
`--pt-scene`, fixed in `e2830db` ("a manifest that silently outranked the command line") by making
that one flag sticky against the manifest. That fix repaired the instance and left the rule
unrepaired: every other `RENDER.*` knob kept the identical bug, because each was its own `if` inside
`applyProjectRenderSettings`, not an application of a shared precedence rule. This fix closes the
rule instead: the manifest still applies first (so an untouched flag still gets the manifest's
value), then every override variable is re-diffed against what the manifest just set, and any
disagreement is reapplied **and logged** —
`[Sandbox] --rt-render-mode: the command line asked for 0 and the project manifest for 1; the command
line wins` — before the frame that renders with it. The equivalent gap in the path-tracing want-flag
(a manifest explicitly stating `RENDER.PATHTRACING` Off used to silently beat `--pt-scene` with no
message at all) was closed the same session, by the same rule.

**The lesson, stated generally so it does not need a third instance:** when a flag exists so a human
standing at the keyboard can override recorded state, the override must be applied **last**, or
reasserted after whatever would otherwise clobber it — and a disagreement between the two must be
**logged**, not silently resolved in either direction. Silence is the only thing that turns a
one-line precedence bug into hours of confidently wrong measurement; a single log line would have
made the original raster-vs-ray-driven comparison self-correcting the first time someone read the
console. See `SESSION-STATE.md` for which numbers from that investigation are now retracted and what
the corrected ones are.

- **Transmission never left the alpha channel, so glass scattered light it had already let through**
  — HIGH, fixed 2026-08-29. Authored glass rendered as a flat, milky, ~80%-opaque wash that hid the
  pool beneath it and looked identical from every angle — the "translucent things look terrible and
  cartoonish" complaint. `PARAM transmission 0.92` reached the GPU correctly and was read in exactly
  one place: `PbrShaders.cpp:456`, where it floors `s.alpha`. **Nothing ever reduced the diffuse
  lobe.** A pane authored 92% transmissive still scattered its full base colour back at the camera —
  the same photons counted twice, once through the substrate and once off it.

  **Bisected, not reasoned about**, because four earlier candidates for this were all wrong. Probing
  one pixel of pane against the water beside it and cutting `averBlendedOutput` down term by term:

  | output | probe | attribution |
  |---|---|---|
  | full | `103,124,130` | — |
  | specular removed | `99,120,125` | the whole specular term is **4 codes** |
  | coverage only | `27,56,84` | vs `0.88 * water(32,69,94)` = `28,61,83` — **correct** |

  So `s.alpha`, the Fresnel lift on it, and the premultiplied blend state were all verified correct
  first, and the specular reflection was never the problem. `diffuse * s.alpha` alone contributed
  `(72,64,41)` — a warm wash five times the size of the reflection. After scaling `kdAlbedo` by
  `(1 - transmission)` (matching glTF `KHR_materials_transmission`): `41,70,95` against bare water
  `32,69,94`, i.e. a nearly-clear pane with a faint reflective lift.

  **The view-angle Fresnel came back with it**, which is the level's own stated acceptance test
  (`Default.ocmap`: *"nearly clear looked at square, nearly a mirror looked at down its length. If
  those two views look the same, the view-angle Fresnel is not reaching the shader and that is the
  bug"*). Same pane, near-normal vs grazing: **before** `194,206,209` / `178,192,196` — flat, and
  slightly the wrong way. **After** `81,97,112` / `120,128,125` — bluer and seeing through at normal
  incidence, brighter and neutral-sky at grazing. It had been flat because both angles were saturating
  past display white, not because the Fresnel term was missing.

  Applied in **two** places on purpose — `PbrShaders.cpp`'s `averBuildSurface` and its ray-hit twin at
  `VoxiShaders.hpp`'s `averEvalMaterial` — because a rule the rasteriser honours and primary rays do
  not is the defect shape this tree keeps rediscovering. Gated on the **material field**, not on the
  blend mode: an opaque material may legitimately author transmission, and it is a substrate property.
  A strict no-op for every material authored before the field existed (`transmission` defaults to 0);
  `M_Glass.ocmat` in PTTest is currently the only material in any project that sets it.

  **Still open, and visible in the same captures:** a translucent surface casts no shadow at all.

- **Three features, one scene, and nobody said who won** — HIGH, fixed 2026-08-29. The entry above
  said the lesson was stated generally "so it does not need a third instance". It got one anyway, in
  a different mechanism, and it invalidated the replacement measurement as thoroughly as the first
  bug invalidated the original. Three defects, all in the same family — *a switch that silently does
  something other than what it says*:

  1. **`PtSceneView::suppressesScene()` returned `true` unconditionally** (`PtSceneView.hpp`). Only
     the FIRST claimant in registration order paints (`D3D12Device::beginFrame` returns at it), and
     `drawMesh` then early-returns for every object. On a project whose manifest sets
     `RENDER.PATHTRACING` — PTTest does — turning ray-driven off did **not** fall back to the
     rasteriser: Voxi stopped claiming, the path tracer was next in line, and the "raster" half of the
     comparison presented a fullscreen path-traced blit while rasterising nothing. It now tests the
     same four terms `scenePass` already guarded on, so it can no longer suppress the scene and then
     paint nothing either.
  2. **`--gi 0`, `--rt 0` and `--pt 0` were silent no-ops.** The sentinel for "flag absent" was `0`,
     which is also `Quality::Off`, so the Off rung was unreachable from the command line and the
     path-traced view could not be turned off at all. The limitation was *written down in a comment*
     rather than fixed — twice, at both the startup site and in `applyProjectRenderSettings`. Worse,
     `--gi` and `--rt` parsed no numeric form whatsoever, so `--gi 2` set High and left `2` to fall
     through to the positional handler. All three now use `-1` and take an optional tier;
     `--no-gi`/`--no-rt` stay as the shorter spelling.
  3. **No GPU marker existed on the raster path's own pixel shading.** The 3x compared
     `Voxi ray-driven primary` — a real, named, correctly-nested span — against `scene draw`'s
     leftover exclusive time, which is not the same category of number. `raster scene draws` now
     brackets the opaque walk in `SandboxApp::onRender`.

  **The tell was in the printed output the whole time, again.** `scene draw 0.1ms (excl 0.0ms)`:
  exclusive is inclusive minus the sum of children, so a span with no children prints them equal.
  `excl 0.0` against `0.1` inclusive means a child consumed all of it — i.e. the raster draws cost
  under 0.05ms, which is impossible for sixteen objects at Epic-tier GI. The number said "raster did
  not run" and was read as "raster is fast".

  **The general fix, since naming the rule evidently was not enough:** the backends now log who is
  painting the scene, on change, **including the single-claimant case** — which is the one that
  actually cost the measurement, and which a "warn only on conflict" check would have missed:
  `[RHI.D3D12] 'Aver.PathTracer.SceneView' is painting the scene; the rasteriser's drawMesh calls are
  being dropped. Any 'scene draw' timing below is that feature, not raster.` A feature that cannot
  paint this frame should answer `false` rather than rely on losing a race it cannot see.

## Open

- **The GI voxelisation gate ignores compute-skinned pose changes** — MEDIUM, **no longer frozen; now
  a documented exclusion instead.** This entry used to say the gate's `giDrawsKey()` never checked
  `meshVertexBuffer`, so a skinned character's indirect-light contribution froze at whatever pose was
  current when its transform last changed. `modules/render.voxi/src/VoxiRenderer.cpp` (rebuild gate
  at `806-832`; the actual fix at the `voxelizePass`/`giDrawsKey` call sites, currently ~1253-1257 and
  ~2328-2353, since this file's line numbers move) now **excludes** any draw whose
  `dev_->meshVertexBuffer(d.mesh)` is non-zero from both the hash and the injection pass, with the
  code's own comment naming this "a trade rather than a fix": a skinned character now bounces **no**
  indirect light at all, consistently, rather than a stale pose's worth of it silently. Measured on a
  real rig at 2 rebuilt / 62 skipped of 64 ticks through a pose transition — the ~96% rebuild-avoidance
  this gate exists for is preserved. **Still open:** the comment names the real fix as partial
  revoxelisation (injecting one mesh's region without rebuilding the whole volume), which
  `voxelizePass` does not support yet.

- **`--refl-test` FAILS, and has been failing** — MEDIUM, found 2026-08-27.

  **Trigger: `Sandbox.exe --refl-test --frames 120`.** The engine's own reflection oracle reports:

  ```
  [ERROR] [Refl] FAIL (not global): hiding a beacon 5200 cm away -- 4.3x outside the voxel volume --
  changed no mirror pixel under RAY TRACING (largest 0.0353)
  ```

  The test builds a near-mirror quad (metallic 1, roughness 0.03) and a bright green beacon 5200 cm
  away — 4.3x outside the voxel volume — then hides the beacon and asserts that a ray-traced
  reflection notices, because a ray, unlike a cone trace, has no volume bound. Nine of nine probes
  read back in all four phases and not one moved by more than 0.0353.

  **Falsified against the reflection work landed the same day**, which is why it is filed as
  pre-existing rather than as a regression: the tree was reverted to `5657ba1`'s `VoxiShaders.hpp`,
  rebuilt, and re-run. It failed identically there (0.0353 at HEAD versus 0.0431 after the change —
  the new code is marginally *more* responsive, not less).

  The beacon **is** in the geometry table (`ray-traced reflection table: 2 instances`), so the two
  candidate explanations are that the probes do not land where the beacon's reflection actually
  falls — the same class of defect as the `rt-penumbra` probe that sat on flat ground for weeks — or
  that something really does bound the ray. **Screen the probe placement first**; a failing oracle
  whose probes are in the wrong place is a broken oracle, not a broken renderer, and this one has
  been reporting FAIL loudly enough that nobody has been reading it.

- **The spatial shadow denoiser is a permanent no-op in the shipped product** — LOW, found
  2026-08-27, **fixed 2026-08-29 in `31c06a4`.** `modules/render.voxi/src/Voxi.cpp`.
  `rtShadowDenoiseForQuality` used to return **0 at every tier including Epic**, so radius 0 hit
  `rtShadowSpatial`'s early return and the filter never executed in anything shipped. It now returns
  **2 for Low/Medium/High and 1 for Epic** (Off stays 0 — RT is not running), with the ladder's own
  comment recording why Epic gets *less* radius than the tiers below it: radius 3 is "within one code
  of the sixteen-ray answer" but also the worst point on a measured 12-to-32-code motion-wobble range,
  so it stays reachable only through an explicit override rather than as any tier's default. What was
  never a bug, and still is not one: `rtPixelsPerRayTileForQuality` still returns 1 at every tier — the
  same code comment now explains this is a **measured, deliberate** choice rather than an oversight
  killing the reuse path (a moving-camera test showed the reprojected-history tile widths visibly
  trailing the shadow during Play-in-Editor, which a still-camera benchmark could not see). See
  `docs/rendering/DENOISING.md`.

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
  *(2026-08-30: that fix covered ONE of the three. The wait-failure exit and the generic
  overlapped-read-failure exit both still left the loop with `died_` untouched, so `watching()` kept
  answering true for a dead worker — the exact thing `died()` exists to prevent, still live in two
  of the three places its own comment named. Every exit now goes through one `noteExit()` that
  decides by asking whether a stop was actually requested. Reachable from a test at last via
  `AVER_WATCHER_KILL_AFTER`, and `WatcherTest` fails without the fix.)*
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
