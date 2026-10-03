# Aver Frame Interpolation — design (draft for review)

**Status:** design. Stage 0 (object motion vectors) is built and committed (`4ee7e3ec`); everything else is
unbuilt. Every millisecond figure is an estimate unless it says *measured*. Revised 2026-10-03 after the
patent sweep ([FRAME_INTERPOLATION_PATENTS.md](FRAME_INTERPOLATION_PATENTS.md)) to the **patent-aware
design (option b)**: written in-house, no FSR3 code.

**One-line summary:** generate one frame between every two real frames by *gathering* each output pixel
from both real frames along their own motion vectors and depth (procedural, exact for most pixels), score
each candidate with one continuous confidence from real-frame consistency, and let a small network choose
only the blend weight. It runs on the HDR scene image **before** the AverSR upscale and before tone mapping;
UI is drawn on top afterwards; frames are presented on a fixed vsync cadence.

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
| 7 | **Motion vectors for moving objects** first — **done** (§4, `4ee7e3ec`). |
| 8 | **Editor while editing** is an **Editor Preferences** setting ("Frame generation in viewport while editing", off by default). Play / PIE and the runtime follow the project's render settings. |
| 9 | **Hardware agnostic, written in-house (option b).** FSR3 is a *reference only*: no FSR3 code is vendored or derived. Plain compute shaders on any D3D12 GPU; no vendor SDK, driver extension or matrix hardware. |
| 10 | **Patent-aware design**: the four changes in §2.3 remove claim elements found by the sweep. Counsel confirms before shipping. |

---

## 2. Background

### 2.1 Engine facts (from the source)

- **Motion vectors:** RG16F, pixels, destination minus source. Since `4ee7e3ec` the ray-driven path carries
  object motion (`RtInstance::prevObjectToWorld`) and the sky carries the camera's rotation; raster stays
  camera-only.
- **The G-buffer is off by default.** It is enabled only when the denoiser wants it
  (`RenderSettingsResolver.hpp:322`; `GameApp.cpp:1897`; `SandboxApp.cpp:2595`). Frame interpolation
  becomes a second reason. It needs single-sample rendering (`D3D12Device.cpp:3949`).
- **There is no history.** The HDR scene target (`msaaColor_` / `sceneResolved_`, RGBA16F) is overwritten
  every frame; the G-buffer is cleared every frame.
- **One Present per frame.** FLIP_DISCARD, 2 buffers (`kFrameCount`, `D3D12Device.cpp:51`), no waitable
  object (`:5963-5974`). `beginFrame`/`endFrame` cannot simply run twice.
- **No scene-cut signal.** History resets are editor-only (`SandboxApp.cpp:2636-2681`).

### 2.2 What existing work does

- **AMD FSR3 frame interpolation** (FidelityFX v1.1.4): fully procedural, 9 passes — a midpoint
  motion field built by splatting game motion with atomics, an interpolated-depth estimate, inpainting
  pyramids, 8×8 block-matching luma flow, two disocclusion masks, blend, inpaint. **Reference only**: AMD
  has pending applications on several of these passes (§2.3).
- **Research on rendered content** (Briedis 2021/2023, Ha/Ahn/Yoon 2025): networks learn flows, occlusion
  weights or kernels, never the image directly; none is real-time.
- **Bidirectional scene reprojection** (Yang, Tse, Sander, Lawrence, Nehab, Hoppe, Wilkins, SIGGRAPH Asia
  2011): builds an in-between frame from two rendered frames by a per-pixel *gather* — fixed-point
  iteration through each frame's own motion and depth. Prior art to the frame-generation filings and the
  basis of this design.

### 2.3 The four patent-driven changes

| Old design | Problem (see the patents doc) | New design |
|---|---|---|
| Splat frame N's motion to the midpoint, pick one per pixel by depth | AMD US 2025/0191120 (pending) | **Gather** (§3.2): each output pixel searches each real frame along that frame's own motion. No midpoint motion field is built; nothing is weighted or selected per output location. |
| Two disocclusion masks (occlusion / dis-occlusion), interpolated depth, snap to one frame | AMD US 2025/0069319 (pending), NVIDIA US 12,568,184 (granted) | **One continuous confidence per candidate** (§3.3) from real-frame consistency only. No mask pair, no midpoint depth, never snapping by mask. |
| Network outputs blend weight **and** a colour fill | Arm GB 2620919 (granted UK), US 2024/0029196 (pending) | **Network outputs only the blend weight** (§3.5). Uncovered pixels are filled procedurally in colour space (§3.4). |
| Pacer times the generated frame from measured frame times | AMD US 2025/0299287 (pending) | **Fixed back-to-back vsync presents** (§5): no frame-time estimator in the schedule; generation off on variable-refresh / uncapped output. |

Two very broad NVIDIA applications (US 2022/0038653, 2022/0398751) have no clean design-around on their
literal text; they are tracked, with prior art on file. The hole fill (§3.4) works in colour space, not by
propagating motion, which keeps distance from the second one.

---

## 3. Pipeline

```
real frame N rendered (HDR scene colour, depth, motion vectors, trust mask)
      |
      |  history ring keeps N-1's colour / depth / motion
      v
G1  gather     per output pixel: fixed-point search into N and into N-1         procedural
G2  confidence one continuous score per candidate from real-frame consistency    procedural
G3  blend      weight per pixel (heuristic in milestone 1, network in 2)         procedural / neural
G4  fill       push-pull colour pyramid for pixels with no valid candidate       procedural
      |
      v
generated HDR frame  ->  AverSR upscale  ->  composite (exposure, bloom, ACES, gamma)
                                         ->  editor lines, overlays, HUD, ImGui  ->  vsync Present
```

### 3.1 Where it sits

- **Insertion point:** between the bloom block and the AverSR block in `runPostChain`
  (`D3D12Device.cpp:5359-5368`), on the scene-resolution HDR image (a quarter of the output pixels at
  AverSR Performance).
- **A generated frame re-runs** only AverSR (spatial, `needs() == None`) and the fused composite.
- **It must not re-run** eye adaptation (`CSExposure` adapts per call with `frameSeconds_`); it uses the
  average of the two real frames' exposure. Local exposure and bloom are blended from the two real frames.
- **With a future temporal upscaler** interpolation moves after the upscale, so the insertion point is a
  setting from the start.

### 3.2 G1 — gather (bidirectional reprojection)

For output pixel `q` at `t = 0.5`:

- **From frame N** (motion `MV_N`, destination minus source): the surface point at `p` in N was at
  `p − MV_N(p)` in N−1, so at the midpoint it sits at `p − 0.5·MV_N(p)`. Solve `q = p − 0.5·MV_N(p)` by
  fixed-point iteration `p ← q + 0.5·MV_N(p)`, starting at `p = q`, 3–4 steps.
- **From frame N−1** (its own motion `MV_{N−1}`, kept in the history ring), assuming constant velocity
  over two frames: `r ← q − 0.5·MV_{N−1}(r)`, starting at `r = q`.
- Each search reads **only that frame's own** motion and depth. No midpoint field is written; there is no
  per-output-location candidate set and no selection.
- Several starting points (e.g. `q` and `q ± 0.5·MV` of the 4-neighbourhood) catch thin moving objects;
  the converged candidate with the best confidence (§3.3) per frame is kept. *(Counsel to confirm a
  per-frame choice among search starts is not the claimed "selection among motion vectors landing at a
  location" — it selects among search results in one real frame, not among vectors of the interpolated
  frame.)*

### 3.3 G2 — one confidence per candidate

Each candidate (from N, from N−1) gets one continuous score in [0, 1] from:

- **Convergence** of its fixed-point search (residual `|q − p + 0.5·MV(p)|` in pixels).
- **Real-frame depth agreement:** reproject the candidate into the *other real frame* with its motion and
  compare against that frame's stored depth (relative, ~1–3%). Never against an interpolated depth.
- **Colour agreement** between the two candidates (low weight; disagreements are often shading changes,
  not errors).
- **The engine trust mask** (glass, water, particles, emissives) lowering both.

There is no occlusion/dis-occlusion mask pair and no snapping: a candidate that is hidden in the other
frame simply has low confidence, and the blend follows continuously.

### 3.4 G3/G4 — blend and fill

- **Milestone 1 blend:** `out = (c_N·w_N + c_{N−1}·w_{N−1}) / (w_N + w_{N−1})`, weights from §3.3.
- **Fill:** pixels where both confidences are near zero (true holes) are filled by a push-pull colour
  pyramid of the blended frame (Gortler et al. 1996): build a pyramid of valid colour, then pull back
  down. Colour space only; motion is never propagated into holes.

Milestone 1 is shippable on its own and is the baseline the network must beat. Estimate ~0.4–0.8 ms at
full 1440p, roughly a quarter at 0.5 scale.

### 3.5 Milestone 2 — the network (weight only)

- **Job:** one output channel per pixel: the blend weight between the two gathered candidates (optionally
  a small search-start offset). **No colour output of any kind.**
- **Inputs (~16 channels, all from the two real frames):** the two gathered colours and their difference,
  each real frame's own depth at its candidate, the two confidences, the trust mask, motion magnitude,
  convergence residuals.
- **Shape:** a 3-level U-Net at 1/4 resolution, 12/16/24 channels (~2.5–4 GMAC at full 1440p, less at 0.5
  scale), weights upsampled with a joint-bilateral filter. fp32 first; fp16 behind the native-16-bit cap.
  *(Intel US 2025/0225705, mixed-precision interpolation network, is MEDIUM: re-read before enabling fp16.)*
- **Runs on:** `Aver.Render.Neural`, which gains conv forward kernels.
- **Training (in-engine, check with the owner first):** ground truth from the engine rendering a scripted
  sequence at double rate with a fixed timestep (half steps are targets). Needs frame determinism first.
  Losses: L1 on tone-mapped colour, Laplacian-pyramid edge term, temporal flicker term. ~20–50k crops from
  10–20 camera paths over the showcase levels, one held out.

---

## 4. Prerequisite: motion vectors for moving objects — DONE (`4ee7e3ec`)

- `RtInstance` gained `prevObjectToWorld` (96 → 160 B, C++/HLSL/stride in step).
- Full builds carry last frame's drawn transforms forward per (mesh, drawBinding) group: exact match
  (static, zero motion), else nearest unmatched within 20 m, else zero motion.
- The mover patch lane saves the old transform before overwriting it, and pairs identical movers by
  nearest translation instead of draw order.
- A prev that differs from current is settled back on the next frame (one upload, no TLAS refit).
- The shader applies the object-space delta between the two transforms to the hit position (exactly 0 for
  static); the sky gets rotation-only motion.

**Still later:** skinned/soft-body deformation (previous pose), raster object motion (root-constant
budget), translucent layers (by design no motion/depth — the trust mask covers them), foliage wind.

---

## 5. Presentation and pacing (patent-aware)

- **Swapchain:** created with `FRAME_LATENCY_WAITABLE_OBJECT`, 3 buffers, `SetMaximumFrameLatency(1)`,
  with the buffer count decoupled from `kFrameCount = 2` (`D3D12Device.cpp:3882-3888`).
- **Present pass split:** `endFrame` becomes "render the scene" + "present pass" (upscale, composite,
  overlays, UI, Present), so the present pass runs for the generated frame and then the real one.
- **Fixed cadence, no measured timing:** with frame generation on, the generated frame and the real frame
  are queued as **consecutive vsync presents** (interval 1), generated first. The display's refresh is
  the clock. There is no frame-time estimator, no GPU or UI timing in the schedule, no pacer thread
  deciding when to present.
- **Frame cap** at half the refresh rate (the real-frame rate the cadence needs).
- **Variable refresh / tearing / uncapped:** frame generation is **off** in these modes rather than
  adapting to them.
- **Below 30 fps base:** warning in the stats overlay (decision 3); the cadence stays the same (the real
  frame simply holds for more vblanks).
- **Latency reduction is separate:** the waitable object with latency 1. A CPU frame-start delay from
  measured GPU time is **on hold** until Intel US 12,057,090 ("Frame pacing…", delaying CPU work to align
  with GPU availability) has been read — the follow-up sweep covers it.
- **Factor:** shaders take `t` as an input; v1 ships 2× only.

---

## 6. UI, editor, scene cuts

- **Runtime:** the game HUD (`UiRenderer`) keeps its draw list until the next submit
  (`UiRenderer.cpp:170-237`), so it is drawn again on the generated frame. Its upload ring is 3 deep
  (`UiRenderer.hpp:49`) against up to 4 writes with two presents per frame: grow it, or key it to the
  presented-frame index.
- **Editor:** the generated frame goes into the viewport texture (`D3D12Device.cpp:5466-5491`), then editor
  lines, overlays and ImGui draw on top.
  - **Editor lines** are cleared by an RAII clearer in `replay` (`EditorLines.cpp:392`): keep the queue
    until the frame's last present.
  - **ImGui** must render once per presented frame; whether its backend tolerates two renders per
    `NewFrame` is unverified.
  - **Picking, gizmos and input** keep running at the real rate; only display doubles.
  - **Editor Preferences** toggle for while-editing (decision 8).
- **Scene cuts:** a device-level cut counter (`IDevice::noteSceneCut()`) raised by level load,
  respawn/teleport, pawn-to-camera, resize, render-scale and MSAA changes; an automatic camera-jump
  detector (position delta, view angle, FOV); a content check (mean gather confidence too low). On a cut,
  present the real frame only. `IUpscaler::reset()` is called from the same path.

---

## 7. Patents

Full sweep, element mappings and design-arounds: [FRAME_INTERPOLATION_PATENTS.md](FRAME_INTERPOLATION_PATENTS.md)
(engineering mapping, **not legal advice**; a freedom-to-operate opinion is still required). This design
applies the four changes in §2.3. A follow-up sweep (2026-10-03) re-checks the HIGH items verbatim against
this revised design and covers the gaps (NVIDIA DLSS frame-generation filings, Intel US 12,057,090, AMD
latency filings, and whether the gather and push-pull replacements are themselves claimed).

---

## 8. Stage plan

| Stage | Deliverable | Neural |
|---|---|---|
| 0 | Object motion vectors, sky motion — **done** (`4ee7e3ec`) | — |
| 0b | G-buffer reason for frame interpolation; scene-cut signal | — |
| 1 | History ring, present-pass split, waitable swapchain, fixed vsync cadence; runtime first, then the editor viewport; HUD/editor-lines/ImGui redraw | — |
| 2 | Milestone 1: G1 gather, G2 confidence, G3 heuristic blend, G4 push-pull fill; exposure and bloom blending | — |
| 3 | `Aver.Render.Neural` conv forward kernels; weight-only U-Net; in-engine training (**check with the owner first**) | Yes |
| 4 | Translucent motion, reflection motion, skinned previous pose, 3× generation | Partly |

---

## 9. Measurements needed (owner runs)

1. Motion-vector validity on a pan and with movers (now that stage 0 is in).
2. Determinism: two identical renders must match before training data can be generated.
3. Cost of G1–G4 at 0.5 scale on the RX 7800 XT (`--gpu-timing`).
4. Native 16-bit ops support (kernel variants).
5. Cadence stability with the waitable swapchain at 3 buffers, vsync on.
6. Per-category error maps (shadows, reflections, glass, particles, foliage).

---

## 10. Open questions

| # | Question | Leaning |
|---|---|---|
| Q1 | Editor while editing | Decided (8): Editor Preferences toggle, off by default |
| Q2 | HUD animation at the real rate or interpolated | Real rate (redraw the same HUD) |
| Q3 | Generated frames in screenshots, `--frames` captures and gates | Bypassed: real frames only |
| Q4 | Skip generation when the real rate already meets the refresh | Off by setting, not adaptively (an adaptive switch would read measured timing) |

---

## 11. Sources

- Yang, Tse, Sander, Lawrence, Nehab, Hoppe, Wilkins, *Image-based Bidirectional Scene Reprojection*,
  SIGGRAPH Asia 2011.
- Gortler, Grzeszczuk, Szeliski, Cohen, *The Lumigraph*, SIGGRAPH 1996 (push-pull).
- AMD FidelityFX SDK v1.1.4 — reference only.
- Briedis et al. 2021/2023; Ha, Ahn, Yoon 2025; RIFE, IFRNet, FILM, EMA-VFI — ideas only.
- Microsoft DXGI docs: flip model, waitable swapchain.
- Patents: see [FRAME_INTERPOLATION_PATENTS.md](FRAME_INTERPOLATION_PATENTS.md).
- Aver source: `voxi.hlsl`, `VoxiRenderer.cpp/.hpp`, `voxi_rt.hlsli` (stage 0); `D3D12Device.cpp:51,
  3882-3888, 3949, 5039-5491, 5842-5974`; `UiRenderer.cpp:170-237`; `EditorLines.cpp:392`.
