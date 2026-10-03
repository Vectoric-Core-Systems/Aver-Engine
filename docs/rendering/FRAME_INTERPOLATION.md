# Aver Frame Interpolation — design (draft for review)

**Status:** design only. No code exists yet. Every millisecond figure is an estimate unless it says
*measured*; nothing in this document has been run. Written 2026-10-03 from two rounds of read-only
scouting (algorithms and patents, a hybrid design, presentation and pacing, and the Aver source).

**One-line summary:** generate one frame between every two real frames by warping both real frames to
the midpoint with the engine's own motion vectors and depth (procedural, exact for most pixels), then
let a small network decide how to blend the two warps and fill what neither covers. It runs on the HDR
scene image **before** the AverSR upscale and before tone mapping, and UI is drawn on top afterwards.

Companion design: [RADIANCE_CACHE.md](RADIANCE_CACHE.md) — both share the `Aver.Render.Neural` module.

---

## 1. Decisions taken (owner, 2026-10-03)

| # | Decision |
|---|---|
| 1 | **Interpolation, not extrapolation.** Two real frames in, the in-between frame out. |
| 2 | **Runs in both hosts**: the packaged runtime and the editor. The editor's viewport-texture path gets fixed rather than excluded. |
| 3 | **Below 30 fps base**: flagged with a visible warning, not switched off. |
| 4 | **UI is drawn after interpolation**, on top of both real and generated frames. UI is never interpolated. |
| 5 | **HDR, before tone mapping, before the AverSR upscale.** AverSR is spatial and keeps no history, so a generated frame can be upscaled like a real one. |
| 6 | **Training runs in-engine.** It needs convolution backward passes in `Aver.Render.Neural`; **check with the owner before starting that work.** |
| 7 | **Fix motion vectors for moving objects** as the first deliverable (§4). |
| 8 | **Editor while editing** is an **Editor Preferences** setting ("Frame generation in viewport while editing", off by default). Play / PIE and the runtime follow the project's render settings. |
| 9 | **Hardware agnostic, with FSR3 as the starting point.** The procedural passes start from AMD FSR3 frame interpolation (FidelityFX SDK v1.1.4, MIT), ported to Aver's RHI as plain compute shaders that run on any D3D12 GPU, then adapted (engine motion first, our trust mask, our network). No vendor SDK, driver extension or matrix hardware is required. |
| 10 | **Patent check before the design is locked:** a claim-by-claim sweep with design-arounds (§7), for counsel to confirm. |

---

## 2. What the scouts established

**The engine side is most of the work.**

- **Motion vectors are camera-only.** `averGBufferVelocity(wpos)` projects one world position through
  this and last frame's camera (`voxi.hlsl:1683-1710`), and its own comment says a moving object gets
  zero motion silently (`:1678-1682`). Sky and miss pixels also write 0 (`:2535`, `:2591`), so camera
  rotation leaves the sky with no motion.
- **The G-buffer is off by default.** It is enabled only when the denoiser wants it
  (`Settings::denoiser`, `RenderSettingsResolver.hpp:322`; `GameApp.cpp:1897`; `SandboxApp.cpp:2595`).
  Frame interpolation must become a second reason to enable it. It also needs single-sample rendering
  (`D3D12Device.cpp:3949`).
- **There is no history.** The HDR scene target (`msaaColor_` / `sceneResolved_`, RGBA16F) is overwritten
  every frame and has no TextureHandle. The G-buffer is cleared every frame. There is no previous depth.
- **One Present per frame, no pacing.** FLIP_DISCARD, 2 buffers (`kFrameCount`, `D3D12Device.cpp:51`),
  no waitable object, `Present(interval or 1)` (`:5963-5974`). `beginFrame`/`endFrame` cannot simply run
  twice, because that would re-run the scene setup and clear the G-buffer.
- **No scene-cut signal exists.** History resets are editor-only (`SandboxApp.cpp:2636-2681`), and
  `IUpscaler::reset()` is never called by anything.

**The interpolation itself is well understood.**

- **AMD FSR3 frame interpolation** (FidelityFX SDK v1.1.4, MIT, `Copyright (C) 2024 AMD`) is entirely
  procedural: 9 passes (interpolated-depth estimate, game-MV field with atomics, inpainting pyramids,
  8×8 block-matching optical flow on a luma pyramid, two disocclusion masks, blending, inpainting, UI
  cleanup). It is the **starting point** for the procedural part (decision 9): its passes are ported to
  Aver's RHI and adapted, keeping AMD's MIT notice on any file derived from it. The MIT licence carries no
  express patent grant, so the patent sweep (§7) checks AMD's own patents against these passes.
- **Research on rendered content** (Briedis 2021/2023, Ha/Ahn/Yoon 2025) uses networks that learn
  flows, occlusion weights or kernels — never the image directly — and none of them is real-time.
- **Generic video interpolators** (RIFE MIT, IFRNet MIT, FILM Apache-2.0, EMA-VFI Apache-2.0) cost
  10–70 ms class even at low resolution. They are useful only for ideas: coarse-to-fine refinement,
  bidirectional warping, splatting with a weight map, training with a teacher that sees the true
  middle frame.

**Aver has one real advantage over driver-level frame generation:** it knows what it draws. It can write
an exact "do not trust motion here" mask for glass, water, particles and emissives, and give reflections
their own motion (the reflection reprojection already exists, `voxi.hlsl:1113-1134`).

---

## 3. Pipeline

```
real frame N rendered (HDR scene colour, depth, motion vectors, trust mask)
      |
      |  keep N-1's colour / depth / motion alive (history ring)
      v
P1  splat      forward-splat N's motion to the midpoint, depth-ordered       procedural
P2  warp+mask  warp N-1 and N to t = 0.5; depth-consistency masks            procedural
P3  features   downsample features to 1/4 res                               procedural
P4  network    small U-Net: blend weight + fill for uncovered pixels         neural (v1.1)
P5  compose    upsample weights guided by full res, blend the two warps      procedural
      |
      v
generated HDR frame  ->  AverSR upscale  ->  composite (exposure, bloom, ACES, gamma)
                                         ->  editor lines, overlays, HUD, ImGui  ->  paced Present
```

### 3.1 Where it sits

- **Insertion point:** between the bloom block and the AverSR block in `runPostChain`
  (`D3D12Device.cpp:5359-5368`), on the scene-resolution HDR image. At AverSR Performance (0.5 scale) that
  is a quarter of the output pixels.
- **What a generated frame re-runs:** AverSR (spatial, `needs() == None`, `AverSrSpatial.hpp:44-45`) and
  the one fused composite pass. That is all.
- **What it must not re-run:** eye adaptation. `CSExposure` adapts once per call using `frameSeconds_`
  (`:5143`, `:5162`); running it twice would double the adaptation speed. A generated frame uses the
  average of the two real frames' exposure.
- **Shared from the real frames:** the local-exposure grid and bloom are blended from the two real
  frames rather than rebuilt (both are low-frequency). Rebuilding bloom is the exact fallback if blending
  shows ghosts on bright moving lights.
- **Later, with a temporal upscaler:** a history-keeping upscaler must not see generated frames, so
  interpolation would move after the upscale. The insertion point is therefore a setting from the start.

### 3.2 Procedural passes (milestone 1, no network)

- **P1 splat:** each pixel of frame N writes its depth-packed index to `q = p − 0.5·MV`. With 64-bit
  atomics (optional on D3D12) one pass does it; without them, a depth-only pass then an index gather.
  A 3×3 nearest-depth fill closes pinholes. The result is a midpoint motion field for surfaces visible
  in N.
- **P2 warp + masks:** sample N−1 and N through the midpoint field. Depth consistency (reprojected depth
  versus stored depth, ~1–3% relative) marks pixels visible in only one frame; those take that frame
  only. The engine trust mask lowers confidence on glass, water, particles and emissives.
- **P5 compose (milestone 1 form):** heuristic weights from the masks and from agreement between the two
  warps. Where they disagree badly, fall back to the nearer frame.

Milestone 1 is shippable as "interpolation lite" and is the baseline the network must beat.
Cost estimate: ~0.5–0.9 ms at full 1440p, roughly a quarter of that at 0.5 scale.

### 3.3 The network (milestone 2)

- **Job:** predict, per pixel, (a) a blend weight between the two warped frames and (b) a colour fill
  for pixels neither warp covers. 4 outputs. It never generates the image itself.
- **Inputs (~20 channels):** the two warped colours, their difference, each frame's own depth at the
  warped position, the disocclusion masks, the trust mask, motion magnitude, and a splat-hole mask.
  All of these come from the two real frames (see the patent notes in §7 for why that matters).
- **Shape:** a 3-level U-Net at 1/4 resolution, 12/16/24 channels, about 2.6–4.5 GMAC per generated
  frame at 1440p; less at 0.5 render scale. Weights are upsampled with a joint-bilateral filter guided by
  the full-resolution difference image.
- **Runs on:** `Aver.Render.Neural`'s portable fp32 kernels (fp16 when the device reports native 16-bit
  ops). It needs **convolution** layers, which the radiance cache does not, so the module grows conv
  forward kernels.
- **Cost estimate:** 0.6–1.2 ms at full 1440p; at 0.5 scale roughly 0.2–0.4 ms.

### 3.4 Training (in-engine — check with the owner before starting)

- **Ground truth from the engine itself:** render a scripted sequence at double rate with a fixed
  timestep. Frames at whole steps are inputs; frames at half steps are targets. No path tracer is
  needed, and the network trains on exactly the kind of image it will see.
- **Blocker to clear first:** two identical renders must match. Frame determinism is an open item in
  the project notes.
- **Losses:** L1 on tone-mapped colour, a Laplacian-pyramid term for edge sharpness, a temporal term
  against flicker.
- **Data:** ~20–50k crops from 10–20 camera paths across the showcase levels (NewSponza, NeonDistrict,
  Jungle, CyberCity), one level held out. Fast pans, orbits, a walking character, driving, particles,
  flickering lights, glass and water.
- **In-engine means** convolution backward passes and the Adam optimiser in `Aver.Render.Neural`,
  reusing the fixed-point gradient accumulation designed for the radiance cache.

---

## 4. Prerequisite: motion vectors for moving objects (first deliverable)

This is the first thing to build. Without it every moving object double-images in generated frames.

**What exists and why it cannot be switched on as-is.** A previous-transform tracker exists
(`rtInstancePrevWorld_`, `VoxiRenderer.cpp:2109-2161`), but it is compiled out (`kTrackPrevTransforms =
false`, `VoxiRenderer.hpp:1255`), never bound to a shader, and turning it on **disables the ray-tracing
mover-patch lane** (`VoxiRenderer.cpp:2414-2421`); the two cannot run together. Its ordinal keying also
zeroes motion for a whole group of repeated props when one instance spawns or despawns.

**Proposed change (ray-driven path, the default `rtRenderMode = 1`):**

1. **Capture the previous transform where it is overwritten.** In `patchRtMovers`, copy the old
   `rtInstanceData_[inst].objectToWorld` into a parallel previous-transform array before writing the new
   one (`VoxiRenderer.cpp:2579-2580`). On a full TLAS rebuild, carry each instance's last transform
   forward by a stable key, so a rebuild frame does not lose motion.
2. **Widen `RtInstance`** from 96 B with a previous `objectToWorld` (4×3 affine, +48 B → 144 B). This is a
   three-place ABI: the C++ struct (`VoxiRenderer.hpp:821-849`), the HLSL mirror (`voxi_rt.hlsli:44-45`),
   and the SRV stride. Update all three together, plus a static_assert on the size.
3. **Use it in the shader.** The primary hit already has the instance in scope (`RtInstance inst =
   rdS.inst`, `voxi.hlsl:2545`). Compute the object-space hit position, push it through the previous
   transform, then through `gPrevViewProj`. Roughly one 4×3 multiply per pixel.
4. **Use the transforms in the image, not this frame's entities.** The draw list the TLAS reads is one
   frame stale by design (`VoxiRenderer.cpp:1222-1240`). Motion must come from successive TLAS
   transforms so it matches what is drawn.
5. **Sky motion.** Give sky and miss pixels a rotation-only reprojection of the view direction instead
   of 0, so camera turns do not smear the horizon.
6. **Retire the dead tracker** (`kTrackPrevTransforms` and its group-population gate) once 1–3 replace it.

**Deliberately later:**

- **Skinned and soft-body meshes** need the previous pose (a ping-pong vertex buffer, or re-skinning with
  last frame's bones). Their rigid motion is covered by step 1; deformation is not, until this lands.
- **Raster path** (`PSMainVoxi`): per-draw transforms live in a 32-dword root-constant block pinned by a
  static_assert (`RHIShaders.cpp:98`); adding 16 dwords touches every root signature. Ray-driven
  is the default, so raster object motion waits.
- **Translucent layers** (glass, water, particles) write no motion or depth by design
  (`VoxiRenderer.cpp:6784-6798`). The trust mask (§3.2) covers them; a separate translucent-motion target
  is a later improvement.
- **Foliage wind** needs the previous wind phase in the shader.

**Also fixes:** any future temporal upscaler and the ray-traced temporal filters get correct object motion
from the same change.

---

## 5. Presentation and pacing

- **Swapchain:** create with `FRAME_LATENCY_WAITABLE_OBJECT`, 3 buffers, `SetMaximumFrameLatency(1)`.
  The waitable flag cannot be added later via `ResizeBuffers`, so it is chosen at creation.
- **Decouple buffer count from frames in flight:** per-frame arrays and the per-backbuffer fence wait
  (`D3D12Device.cpp:3882-3888`) assume both are `kFrameCount = 2`.
- **A new present pass:** `endFrame` splits into "render the scene" and "present pass" (upscale,
  composite, overlays, UI, Present), so the present pass can run once for the generated frame and once
  for the real one.
- **A pacer thread** presents the generated frame at half the base frame time after the previous real
  frame, then the real frame at the full frame time. Base frame time is a moving average minus a variance
  margin (FSR3's estimator). Waits above ~2 ms use an event; shorter ones spin.
- **Defaults:** vsync on, with a frame cap slightly below half the refresh rate. Tearing is an advanced
  option.
- **Latency:** interpolation holds each real frame back by about half a base frame (30→60 fps: ~17 ms;
  60→120: ~8 ms). A vendor-neutral reduction: the waitable object with latency 1, plus delaying the start
  of each frame from measured GPU time so input is sampled as late as possible.
- **Below 30 fps base:** a visible warning in the stats overlay (decision 3). The overlay shows base fps,
  output fps, generation cost and added latency.
- **Factor:** the pacer and shaders take the in-between time `t` as an input, so 3× and 4× are possible
  later. v1 ships 2× only.

---

## 6. UI, editor, scene cuts

- **Runtime:** the only overlay is the game HUD (`UiRenderer`). It keeps its draw list until the next
  submit (`UiRenderer.cpp:170-237`), so it can be drawn again on the generated frame. Its upload ring is
  3 deep (`UiRenderer.hpp:49`) against up to 4 writes with two presents per frame: grow it, or key it to
  the presented-frame index.
- **Editor:** composite and overlays draw into a window-sized viewport texture that ImGui then shows
  (`D3D12Device.cpp:5466-5491`, `SandboxShell.cpp:2489-2497`). The generated frame goes into that texture
  the same way, then editor lines, overlays and ImGui draw on top. Two things to fix:
  - **Editor lines** are cleared by an RAII clearer inside `replay` (`EditorLines.cpp:392`), so a second
    replay draws nothing. Keep the queue until the frame's last present.
  - **ImGui** must render once per presented frame. Whether its backend tolerates two renders per
    `NewFrame` is unverified.
  - **Picking, gizmos and input** keep running at the real frame rate; only display doubles.
- **Scene cuts:** add a device-level cut counter (`IDevice::noteSceneCut()`), raised by level load,
  respawn or teleport, pawn-to-camera, resize, render-scale and MSAA changes. Add an automatic camera
  jump detector (position delta, view angle, FOV). Add a content check inside the interpolator (warp
  error too high). On a cut, present the real frame only. Call `IUpscaler::reset()` from the same path.

---

## 7. Patents (flags, not legal advice)

The scout read the claim text of three patents. Counsel decides; these notes record where the design sits.

- **US 12,367,544** (ETH Zurich / Disney, kernel-based frame interpolation). Every independent claim
  requires a third, intermediate-frame auxiliary buffer, a rendering mask derived from it, **actually
  rendering part of the in-between frame**, and blending that partial render with the interpolated image.
  *This design never renders any part of the in-between frame.*
- **US 12,288,281** (ILM / Disney, frame interpolation for rendered content). Requires neural networks
  applied to feature maps from both key frames **and rendering features of the target (in-between)
  frame**, producing pixel mappings to the target. *This design feeds the network only data from the two
  real frames.* The procedural splat does produce a midpoint motion and depth field, but that stays
  outside the network. Whether a reprojected intermediate depth counts as "rendering features of the
  target frame" is a question for counsel.
- **US 10,776,688** (NVIDIA, multi-frame interpolation with optical flow; the Super SloMo family).
  Concerns a network built on bidirectional intermediate flows and warped inputs. *Here the flow is
  procedural (engine motion), and the network only weights and fills.* Counsel should compare the claims.
- **Not yet searched properly:** DLSS frame generation, AMD FSR3/AFMF, Intel XeSS-FG, Samsung and
  Qualcomm. A proper freedom-to-operate search (Espacenet / USPTO) is needed before shipping.
- **Code:** FSR3 v1.1.4 (MIT) is the starting point for the procedural passes (decision 9); files derived
  from it keep AMD's copyright and MIT notice. Nothing else is vendored.
- **Sweep in progress (2026-10-03):** AMD (including FSR3's own passes), NVIDIA, Intel / Qualcomm /
  Samsung / Apple / Arm, Disney / Sony / Microsoft / Google / Meta, and a technique-first search, each
  reading independent claims verbatim and mapping them to the design's elements, with an adversarial
  second reviewer. Results replace this paragraph.

---

## 8. Stage plan

| Stage | Deliverable | Neural |
|---|---|---|
| 0 | Object motion vectors (§4), sky motion, G-buffer reason for frame interpolation, scene-cut signal | — |
| 1 | History ring, present-pass split, waitable swapchain, pacer thread; runtime first, then the editor viewport | — |
| 2 | Procedural interpolation P1, P2, P5 at the HDR insertion point; exposure and bloom blending; HUD redraw | — |
| 3 | `Aver.Render.Neural` conv forward kernels; P3 + P4 network with weights trained in-engine (**check with the owner before the training work**) | Yes |
| 4 | Translucent motion / trust improvements, reflection motion, skinned previous pose, 3× generation | Partly |

---

## 9. Measurements needed (owner runs)

1. Motion-vector validity: warp N−1 to N with the motion field on a camera pan and on a scene with
   movers; per-pixel error before and after §4.
2. Determinism: two identical renders must match before training data can be generated.
3. Cost of each procedural pass at 0.5 scale on the RX 7800 XT (`--gpu-timing`).
4. Whether the device reports 64-bit atomics and native 16-bit ops (picks the splat and kernel variants).
5. Pacing jitter with the waitable swapchain at 2 versus 3 buffers.
6. Per-category error maps (shadows, reflections, glass, particles, foliage) to size the trust-mask work.

---

## 10. Open questions

| # | Question | Leaning |
|---|---|---|
| Q1 | Editor: interpolate while editing, or only in Play? | Decided (8): an Editor Preferences toggle, off by default; Play follows the project setting |
| Q2 | HUD animation at the real rate or interpolated | Real rate (redraw the same HUD) |
| Q3 | Generated frames in screenshots, `--frames` captures and gates | Bypassed: captures and gates see real frames only |
| Q4 | Skip generation when the real rate already meets the display refresh | Yes, adaptively |

---

## 11. Sources

- AMD FidelityFX SDK v1.1.4 (MIT): frame interpolation passes and the frame-interpolation swapchain.
- Briedis et al., *Neural Frame Interpolation for Rendered Content*, SIGGRAPH Asia 2021; kernel-based
  follow-up, SIGGRAPH 2023. Ha, Ahn, Yoon, Pacific Graphics 2025.
- RIFE (MIT), IFRNet (MIT), FILM (Apache-2.0), EMA-VFI (Apache-2.0).
- Microsoft DXGI docs: flip model, waitable swapchain, frame statistics.
- Google Patents: US 12,367,544; US 12,288,281; US 10,776,688.
- Aver source: `voxi.hlsl:1641-1710, 2535, 2545, 2591`; `VoxiRenderer.cpp:1222-1240, 2109-2161, 2414-2421,
  2579-2580, 6784-6798`; `VoxiRenderer.hpp:821-849, 1255`; `voxi_rt.hlsli:44-45`; `D3D12Device.cpp:51,
  3092-3099, 3882-3888, 5039-5491, 5842-5974`; `UiRenderer.cpp:170-237`; `EditorLines.cpp:392`.
