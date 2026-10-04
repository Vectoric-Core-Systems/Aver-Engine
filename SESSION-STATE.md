# Session state — 2026-08-28

**Read this section before anything below it.** The 2026-08-27 file is folded in further down,
unedited except two lines marked `[CORRECTED 2026-08-28]` inline — its fixed-bug list, its traps and
its gate notes are still true. Its raster-vs-ray-driven picture is not: the comparison that produced
it was measuring one render path against itself. That is now fixed, and this section says what
changed, what the real numbers are, and what is still open.

---

## The flag bug, and why it is the expensive one

**Written up fully in `docs/BUGS.md`, 0.5.0 section, entry 7. Fixed in the working tree
(`SandboxApp.cpp::applyProjectRenderSettings`), not yet committed.**

Short version: CLI overrides (`--rt-render-mode` and eight `RENDER.*` siblings) applied once at
startup; opening a project ran `applyProjectRenderSettings` **afterward** and unconditionally
overwrote them from the manifest, with no log line. `--rt-render-mode 0` against a manifest pinning
`RENDER.RTRENDERMODE 1` silently ran in mode 1. This is the **second** instance of the identical
defect — the first was `--pt-scene`, fixed in `e2830db` by making that one flag sticky — and this fix
closes the rule (manifest applies, then every override is re-diffed against it and reapplied with a
logged disagreement) instead of patching a tenth single case.

**It cost an entire investigation.** A raster-vs-ray-driven comparison run against `PTTest` set
`--rt-render-mode 0` for its raster half. `PTTest.ocproject` pins `RENDER.RTRENDERMODE 1`. The flag
lost, silently, every time. The comparison concluded the two paths were **"pixel-identical"** and
**"at performance parity."** Both conclusions are retracted. There was only ever one render path in
that comparison.

## The corrected numbers — RETRACTED IN TURN, 2026-08-29. See "The numbers that survived" below.

Re-measured with the flag fix in place, PTTest range, identical camera, 16 objects:

| | scene shading | whole frame | probe |
|---|---|---|---|
| raster | 0.1 ms | 2.136 ms | `142,140,136` |
| ray-driven | 3.7 ms | 6.475 ms | `143,139,134` |

Shading is close (3 codes — the earlier all-white bug, `PSRayDriven` building its surface from
`mat.baseColorFactor` alone instead of `inst.albedo * mat.baseColorFactor` the way raster's shader
does, is already fixed and not part of what's still open). **Cost is genuinely 3x worse**, and that
part of the retracted "performance parity" claim needed a real explanation, not just a re-measurement
— see below.

**Every number in the table above is wrong, and the tell was sitting in it.** `142,140,136` is
recorded there as the RASTER probe. It is the path tracer's output: PTTest's manifest sets
`RENDER.PATHTRACING 4`, `PtSceneView::suppressesScene()` was unconditionally `true`, and with
ray-driven switched off the path tracer became the scene's presenter while every `drawMesh` was
dropped. So "shading is close (3 codes)" was comparing the path tracer against ray-driven — two
things that genuinely do agree — and calling it raster-vs-ray-driven parity. `scene draw 0.1ms
(excl 0.0ms)` said the raster draws cost under 0.05ms, which is impossible for sixteen objects at
Epic GI, and was read as "raster is fast" rather than "raster did not run".

## The numbers that survived — 2026-08-29, three defects later

Three separate processes, `--no-vsync`, `--pt 0` so only one feature claims the scene, both paths on
their own GPU marker (`raster scene draws` did not exist before this):

| | scene shading (GPU) | whole frame (CPU median) | probe |
|---|---|---|---|
| raster | **4.3 ms** | 6.389 ms | `65,66,70` |
| ray-driven | **2.3 ms** | 4.403 ms | `143,140,137` |
| path-traced reference | — | — | `142,140,136` |

**Ray-driven is 1.9x FASTER, not 3x slower.** The direction was inverted, not merely the magnitude.

**And the surviving parity gap is the opposite of the assumed one:** the rasteriser is far too dark.
Its floor renders near-black under a bright sky while ray-driven lights it, and the path-traced
reference — the one renderer here with no cheats to get wrong — agrees with ray-driven to a single
code (142 vs 143). Whatever is wrong is in the RASTER path's indirect, not the ray-driven one. Any
plan section aimed at "ray-driven's GI is too dim" is pointed at the wrong renderer.

Consequently retracted from §3 below: the "14 cones x 0.21ms ~= 3.1ms ~= essentially the whole 3.7ms
gap" arithmetic (there is no 3.7ms gap to explain), and the "~1.0ms" structural-traversal figure,
which was a pre-cone-port measurement being quoted as a decomposition of a later number.

## What's real, from three read-only audits run against the corrected picture

Each audit was read-only: no build, no launch, source and one external project manifest only. Full
evidence trails are in the audit output this session was hijacked to accompany; this is the
actionable compression.

### 1. Speckle on the pit far wall — ray-driven only, survives every toggle tried

**Leading hypothesis, falsifiable in one line:** `PSRayDriven` passes a **zero footprint**
(`dpx=dpy=float3(0,0,0)`) to `rtShadowTemporal` (`VoxiShaders.hpp:1683-1684`); `PSMainVoxi` passes
real `ddx(i.wpos)`/`ddy(i.wpos)` (`:1330-1331`). The footprint's only job, per the function's own
comment, is turning the per-pixel shadow test into an area estimate — with it zeroed, every sample in
a pixel fires from the identical origin, so a high-frequency shadow boundary (a grazing self-shadow
off the pit's own rim) aliases into speckle instead of resolving as a soft edge. This is consistent
with every negative toggle in the original report (GI on/off is orthogonal to the shadow call; more
shadow rays only add angular samples from the same fixed origin, which cannot fix spatial aliasing;
`rtShadowDenoise` 0→2 only partially recovers already-aliased data near grazing geometry).

**One-line falsification test, next session, requires a build:** temporarily pass
`ddx(wpos)`/`ddy(wpos)` instead of the zero footprint and re-capture the same PTTest frame. Clears →
confirmed, and the real fix is a bounded ray-safe footprint substitute (a small world-space epsilon
scaled by hit distance), not a raw derivative of a ray-reconstructed position, which the function's
own comment already calls unsound. Does not clear → pivot to the untested second candidate: the
ray-driven shadow-history write and the raster-path glass draw share one `gRtShadowHistOut` UAV for
the whole frame, keyed only by `SV_Position` with no writer tag — if the PTTest glass pane overlaps
the far wall in screen space, its own shadow write clobbers the wall's, failing history reprojection
and forcing perpetual fresh (noisy) retracing there. Whether the pane actually overlaps needs scene
content this audit's constraints didn't allow reading.

**[CORRECTED 2026-08-28]** last night's file said "the speckle in captures is [MSAA absence and no
textures], not the denoiser." That is very unlikely to be the mechanism — MSAA absence and missing
textures make ray-driven shading flatter and smoother, not noisier, and neither is gated by shadow
ray count or denoise strength the way the report says the speckle is. Treat that line as superseded
by the footprint hypothesis above until the one-line test above confirms or kills it.

### 2. Glass — the pane's own shader is fine; what it reveals is not

`PSMainVoxi` renders every blended (glass) draw in **both** modes — traced through
`D3D12Device::drawMesh`/`scenePipeline`, not just through the comment claiming it — so the pane's own
Fresnel, blend, and its own `rtReflectionTemporal` mirror ray are byte-identical raster vs.
ray-driven. What differs is what's *behind* the glass and what a smooth surface *near* it reflects,
both painted by whichever pass answered primary visibility. `PSRayDriven` never calls
`rtReflectionTemporal` at all — deliberately not ported, per its own comment, because it is a whole
extra ray on top of the primary ray, the shadow ray, and the GI bounce loop this pass already pays
for. Its environment specular is `PSMainVoxi`'s own **cone-trace-or-flat-sky fallback**, never the
sharp mirror a smooth raster surface gets.

**The fix, if wanted:** call `rtReflectionTemporal` from `PSRayDriven`'s environment-specular block,
gated behind the same `rough <= 0.75` predicate `PSMainVoxi` already uses. No new pipeline, binding
set, or resource — `rayDrivenPso_` already shares `PSMainVoxi`'s `gi` layout and the
`gRtReflHist`/`gRtReflHistOut` registers are already bound at startup. It should be tiered or toggled
rather than unconditional: cost is real (see §4) and this reuses the exact roughness gate that already
limits which pixels pay for it.

**Water reads "brighter/flatter" in ray-driven for the same reason, not its own bug.** `PSWater`
reads no render-mode state, calls no RT reflection ray, and samples no scene depth — grep across
`modules/fluids` for anything mode-aware returns nothing. It composites premultiplied over whatever
`PSRayDriven` painted underneath it (flatter, no baked texture detail, cone-traced-not-mirror nearby
specular), so the water symptom resolves once the glass/background gap above does, with no fluids
change needed.

### 3. Cost — 3.7ms is mostly PTTest running Epic-tier GI, not a structural ray-driven floor

**Correction to a claim this task's own briefing repeated, which was itself sitting in the tree as a
fresh, uncommitted, already-stale comment:** `giCones` is **not** "SIX, hardcoded, no quality tier
reduces it." A tier ladder already exists, already committed (`Voxi.cpp:220-229`,
`giConesForQuality`, landed in `fbb3aad`), and is already wired live every frame
(`VoxiRenderer.cpp:699`): Off/Medium → 6, Low → 3, High → 9, **Epic → 13**. `PTTest.ocproject` sets
`RENDER.GI 4` — Epic — so the FACTS-block measurement above was taken at **13 cones**, not 6, plus
`RENDER.RTSHADOWRAYS 4` (also Epic). Arithmetic: 14 `traceCone` calls (13 diffuse + 1 ungated
specular-fallback cone) × the measured ~0.21-0.22ms/cone (two independent measurements agree) ≈
3.1ms — essentially the whole 3.7ms gap. The one genuinely structural ray-driven cost — one
`RayQuery` per pixel unconditionally, sky misses included, no early-Z equivalent — measures ~1.0ms on
its own GPU marker (`Voxi ray-driven primary`, SESSION-STATE's own number from last night).

**There is a real, still-uncommitted comment in `VoxiShaders.hpp` around line 1855 that repeats the
false "hardcoded six" claim, twenty lines below the correct one `fbb3aad` already wrote at
`:1143-1148` ("THE RING COMES FROM THE QUALITY TIER NOW, not from a hardcoded 5").** This file
(`SESSION-STATE.md`) and `docs/BUGS.md` are the only files this session was permitted to touch; that
paragraph needs deleting or rewriting by whoever next has `VoxiShaders.hpp` open, per this tree's own
house rule to fix comments you falsify.

**What to actually do:** re-measure the FACTS-block comparison with PTTest forced to Medium tier
(6 cones, 1 shadow ray) before concluding anything about a structural gap. Predicted recovery:
~1.5-2ms, which would collapse most of the "3x." A secondary, smaller item once that lands: the
specular fallback cone is unconditional GI-on cost, ungated by `giCones` — worth folding onto the
ladder (skip below High, or gate by roughness) as a second-order cleanup, not before the tier
re-measurement.

## What to do next, ranked

1. Re-measure FACTS-block cost at PTTest forced to Medium tier — pure measurement, no code change,
   settles whether the "3x" gap is mostly Epic-tier GI (predicted) or something else.
2. One-line footprint test for the speckle (needs a build) — `ddx(wpos)`/`ddy(wpos)` in place of the
   zero footprint at `VoxiShaders.hpp:1683-1684`, re-capture, see if the pit far wall clears.
3. Delete/rewrite the stale "giCones is SIX, hardcoded" paragraph in `VoxiShaders.hpp` (~line 1855) —
   whoever is next in that file, not this session.
4. Decide whether to port a gated `rtReflectionTemporal` call into `PSRayDriven`'s specular fallback
   for glass/water background quality — after (1), since it adds cost to the same budget (1) is about
   to shrink or explain.
5. Re-record gates once the above settle. Also worth checking, not yet checked: whether any **already
   recorded** gate baseline was captured against a project manifest carrying a conflicting `RENDER.*`
   key while the flag bug stood — if so that baseline may need re-derivation too, not just the ones
   already flagged for the ray-driven-default flip (see "Gates" below).

---

# Carried over from 2026-08-27 (previous session)

**Tree clean except the other session's two AverAssetC files and the untracked
`content/dev/test_diffuse_1k.jpg` — never stage those. Nothing is committed yet; it is all in the
working tree so anything can still be changed by editing rather than by a follow-up commit.**

Green at the moment of writing: **89 suites, 0 failed. PT FURNACE PASS.**
**Gates: 7 of 18 failing, expected — see "Gates" below. We re-record together at the end.**

---

## The single most important find of the night

`PathTracer::addScene` did `s.tlas = tlas_` — **every scene shared one acceleration structure.**
That was my own Stage 1 leak fix (`fc11fd1`), correct for `PtSceneView` (one re-armed scene) and
silently wrong for anything holding several at once. `PtFurnaceTest` holds five, so **every furnace
configuration was tracing whichever scene was built last.**

It had been corrupting the path tracer's whole oracle. I nearly filed the symptom as a real defect —
`open/albedo-1` read exactly half energy, and it reproduced at HEAD, so it looked pre-existing and
genuine. Fixed with a TLAS pool indexed by scene ordinal, which keeps the leak fix (a re-arm reuses
slot 0, 1, …) while giving simultaneous scenes their own structures.

```
open/albedo-1/correct   0.250000  ratio 1.000000   (was 0.125000)
cave/4-bounce           0.721863  escaped 0.72  bounces 2.50   (the cave traps again)
cave/32-bounce          0.999832  escaped 0.9998
seed/first == seed/replayed                        (determinism restored)
FURNACE PASS
```

**Lesson worth keeping: a shared-resource optimisation is only correct for the caller you tested.**

---

## What landed

**Glass / translucency.** `AlphaMode::Blend` renders, after being `NotImplemented` since it was
written. Blended draws leave the opaque path at the top of `drawMesh`, are captured, sorted
back-to-front and replayed **after the deferred sky** through a **premultiplied-alpha** pipeline
(straight `over` attenuates the specular reflection by the transparency, which is what made glass
read as flat tint). `averShadeSplit` / `averBlendedOutput` keep the two lobes apart.
`MaterialDesc` gained `ior` + `transmission`; `MaterialConstants` grew 80 → 96 bytes.

**Path-traced dielectrics.** Fresnel-weighted reflect/refract, Snell, total internal reflection,
added *beside* the Lambertian lobe so every original furnace check still exercises identical code.
Exact at IOR 1.5 **and** 2.42, with a new deliberate defect the oracle detects.

**Water** now reflects the real sky with a GGX highlight instead of lerping two hardcoded constants.

**Ray-driven primary visibility is the default.** `rtRenderMode` = 1 in the struct default and at
Low/Medium/High/Epic, 0 at Off. Verified with no flags: `Voxi ray-driven primary 1.0ms`.

**Drone** is a real quadcopter (420 tris, 1 m prop-tip diagonal), verified visually.

**FidelityFX Denoiser vendored** — `third_party/fidelityfx-denoiser`, MIT, 1,677 lines, both
denoisers. **It cannot run yet**; its README lists the exact callbacks and why.

**Motion vectors + thin G-buffer** — velocity RG16F, viewZ R32F, normal+roughness RGB10A2. Behind
`--gbuffer`, off by default. **Verified working end to end.**

---

## Read this before testing in the morning

- **[CORRECTED 2026-08-28] PTTest's render-mode pin, and what it means.** This line originally read:
  *"PTTest will NOT show you the ray-driven default. Its manifest pins `RENDER.RTRENDERMODE 0`
  (`PTTest.ocproject:17`), which correctly outranks an engine default. Change that line or test
  elsewhere."* Two things in it were wrong. First, the manifest now reads `RENDER.RTRENDERMODE 1` —
  it was changed since this was written, whether by hand or by a save. Second, and the part that
  actually mattered: **"correctly outranks"** described the wrong precedence order. A manifest
  correctly outranks the *engine default*. It must not outrank an *explicit CLI flag* — that was the
  flag-precedence bug (`docs/BUGS.md` 0.5.0 §7), now fixed. Anyone testing render mode against PTTest
  from here on: `--rt-render-mode N` on the command line now wins over whatever the manifest says, and
  a disagreement between them is logged.
- **The G-buffer requires `--msaa 1`.** Its targets are single-sample by design (a compute consumer
  will not read `Texture2DMS`), and D3D12 requires every bound target to share a sample count. At
  MSAA > 1 the backend declines to bind them and warns once; `pickGbuf()` was gated to match, or it
  would have selected a 4-target PSO against 1 bound target.
- **Ray-driven has no MSAA and is untextured. [SEE 2026-08-28 CORRECTION ABOVE]** Both are real
  consequences of the default flip. The line that followed this one — attributing the speckle in
  captures to this rather than to the denoiser — is very likely wrong; see "What's real" above for
  the footprint hypothesis that actually explains the speckle's toggle-survival pattern.

### The velocity check, and the one thing that is not perfect

```bash
Sandbox.exe --gbuffer-debug velocity --msaa 1 --probe-rel 0.5 0.5
```

| camera | probe | reading |
|---|---|---|
| still | `128,127,128` | X exactly zero, **Y one code low** |
| `--cam-wobble 5 60` | `255,144,128` | X saturated, Y positive — real motion |

B is a constant 0.5 in both, so R and G are the velocity channels. **X is exactly zero at rest and Y
is not** — about 0.06 texels/frame. Y is the axis with the NDC flip (`0.5 - ndc.y*0.5`), which is
exactly where that class of bug lives. Small, real, and worth chasing before anything consumes the
buffer, because a temporal filter will integrate it.

**Dynamic objects do NOT get true motion vectors yet.** Velocity is camera-only: a moving instance's
own motion reads zero. `VoxiRenderer` has a previous-transform tracker built and measured
(~0.5–1 MB at 6,385 entities) but deliberately not wired, because reaching the shader needs a
coordinated `RtInstance` ABI change across two files that were being edited concurrently.

---

## Two bugs I fixed in the agents' work, both worth knowing

1. **`IDevice::backbufferFormat()` does not return the backbuffer format.** It returns
   `kSceneColorFormat` (RGBA16F). The debug overlay trusted the name, declared RGBA16F, and drew onto
   the RGBA8 composite → 12 debug-layer errors a frame → device removal → access violation.
   `UiRenderer.cpp:105` already sidesteps it by hardcoding `RGBA8Unorm`. **That method should be
   renamed**; it is a trap with two victims now.
2. **A render feature's destructor runs after `onShutdown`**, by which point the resource factory is
   gone. `GBufferDebugFeature` released its pipeline there and crashed on the way out. `FluidScene`
   carries a comment about the identical bug; both are now torn down explicitly in `onShutdown`.

---

## Gates: 7 failing, and why

`rt`, `ms-rt`, `ms-rt-gi`, `shadow-rt`, `shadow-ms-rt`, `penumbra-rt`, `rt-penumbra`. Three moved
from the reflection work (`rt` and `ms-rt` 65,25,18 → 77,33,25; `ms-rt-gi` 64,20,14 → 71,26,20); the
rest moved when ray-driven became the default, because every RT gate now measures a different render
path. **Predicted, not discovered.** `./scripts/record-gates.ps1`, ~25 min per tree, both trees as a
pair.

**Not yet checked, worth checking before the re-record above:** whether any of these baselines — or
any other recorded gate — was captured while running against a project manifest that carried a
conflicting `RENDER.*` key, which the flag bug would have silently resolved in the manifest's favour
regardless of what the gate script's own flags asked for. If a gate script passes `--rt-render-mode`
or similar against a project with its own `RENDER.*` line, the recorded baseline may reflect the
manifest's choice, not the flag's, for the same reason PTTest's comparison did.

## Open, in the order I would take it

- **The Y-velocity residual** above. Cheap, and it blocks trusting the buffer.
- **Dynamic-object motion vectors** — widen `RtInstance` with `prevObjectToWorld`; the CPU tracker is
  already shaped for it.
- **Then** wire the FFX shadow denoiser. Note its shadow path wants a *bitmasked* hit buffer (one bit
  per pixel, packed 8×4) while `rtShadow` returns a *fraction* of the sun disc — thresholding throws
  away the penumbra. Our own filter already consumes a fraction. **This is a real design choice and
  not obviously in FFX's favour.**
- **The selection outline is invisible in the new default** — gated on `sceneSuppressed()`
  (`SandboxApp.cpp:5450`), true in ray-driven mode. Probably half of "gizmos don't show up".
  Plan: `docs/rendering/VIEW_MODES_PLAN.md`.
- **Wireframe under ray-driven** is structurally impossible (a ray pass has no fill mode). Plan
  proposes forcing a raster fallback for that frame rather than greying it out.
- **The horizontal glass slab reads near-black** at steep angles (probed `43,46,49`). Not diagnosed.
- **Unlit + shader complexity** — designed, not built, in the same plan.
- **ABI startup verification** — planned in `docs/ABI_VERIFICATION_PLAN.md`. It concludes honestly
  that **none of the four real incidents it was motivated by would have been caught**, and says why.
- **Ray-driven glass/water background quality and the ray-driven cost gap** — both superseded by the
  corrected 2026-08-28 findings above; see "What to do next, ranked" instead of treating these as
  open in their original form.

## Traps — do not re-pay these

- **Falsify before believing.** Twice tonight a symptom reproduced at HEAD and looked pre-existing;
  once it genuinely was (`--refl-test`), once it was a deeper bug of mine (the shared TLAS). A third
  time, the following morning: a whole render-mode comparison reproduced identically across "both"
  modes and looked like real parity — it was the flag-precedence bug, not the renderer. Restore by
  `cat`, never `cp` — `cp` keeps mtime and Ninja skips the rebuild.
- **A project manifest outranks an engine default, silently and correctly. It must NOT outrank an
  explicit CLI flag — that was a bug, not a design, and it is fixed as of 2026-08-28 (`docs/BUGS.md`
  0.5.0 §7).** The precedence order is flag > manifest > default, all three levels, no exceptions,
  and a flag/manifest disagreement is now logged rather than resolved in silence.
- **`--play-test` runs exactly 150 frames**, holds W, fires from 60.
- **Bare `--pt` does not register the path tracer**; `--pt-scene` does.
- **A legacy project refuses to open under a frame limit** and every number then measures an empty
  editor. Use `--open-legacy`.
- **`Aver.Render.Voxi.dll` not rebuilding is not a stale build** — `Voxi.cpp` (settings) and
  `VoxiRenderer.cpp` (renderer) are separate CMake targets.
- **A tolerance below an estimator's standard error is a check that fails at random.** The furnace's
  new defect check needed 5e-3, not 1e-4; the derivation is in the comment.
