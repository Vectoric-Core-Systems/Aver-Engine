# NeuraFI — Neural Frame Interpolation (Aver) — design

NeuraFI is the engine's frame interpolation: the procedural gather (milestone 1) plus the learned
trajectory path (§3.5). Code: `modules/render.neurafi` (`NeuraFI.hpp/.cpp`, `neurafi.hlsl`), on the RHI's
`IFrameInterpolator` seam. Settings and flags describe the feature: "Frame interpolation",
`--frame-interp`, `RENDER.FRAMEINTERP`.

**Status:** stage 0 (object motion vectors, `4ee7e3ec`) and **milestone 1 (procedural, D3D12)** are built.
Milestone 1 measured on NewSponza at 1766×994 scene resolution (RX 7800 XT, `--gpu-timing`): generation
**0.25 ms**, plus **0.48 ms** for the generated image's own post chain. No debug-layer errors of its own.
Not yet measured: quality against true midpoint frames, cadence stability, the editor viewport path under
interaction. Vulkan: not started. The neural trajectory prior (§3.5) is built and opt-in. Every
millisecond figure is an estimate unless it says *measured*. Revised 2026-10-03 after the
patent sweep ([NEURAFI_PATENTS.md](NEURAFI_PATENTS.md)) to the **patent-aware
design (option b)**: written in-house, no FSR3 code.

**One-line summary:** generate one frame between every two real frames by *gathering* each output pixel
from both real frames along their own motion vectors and depth (procedural, exact for most pixels), score
each candidate with one continuous confidence from real-frame consistency, and let a small network choose
only the blend weight. It runs on the HDR scene image **before** the AverSR upscale and before tone mapping;
UI is drawn on top afterwards; frames are presented on a fixed vsync cadence.

Companion design: [NEURAC.md](NEURAC.md) — both share the `Aver.Render.Neural` module.

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
| 8 | **Editor while editing** is an **Editor Preferences** setting ("Frame interpolation in viewport while editing", off by default). Play / PIE and the runtime follow the project's render settings. |
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
  iteration through each frame's own motion and depth. Prior art to the frame-interperation filings and the
  basis of this design.

### 2.3 The patent-driven changes

| Old design | Problem (see the patents doc) | New design |
|---|---|---|
| Splat frame N's motion to the midpoint, pick one per pixel by depth | AMD US 2025/0191120 (pending) | **Gather** (§3.2): each output pixel searches each real frame along that frame's own motion. No midpoint motion field is built; nothing is weighted or selected per output location. |
| Two disocclusion masks (occlusion / dis-occlusion), interpolated depth, snap to one frame | AMD US 2025/0069319 (pending), NVIDIA US 12,568,184 (granted) | **One continuous confidence per candidate** (§3.3) from real-frame consistency only. No mask pair, no midpoint depth, never snapping by mask. |
| Network outputs blend weight **and** a colour fill | Arm GB 2620919 (granted UK), US 2024/0029196 (pending) | **Network outputs only the blend weight** (§3.5). Uncovered pixels are filled procedurally in colour space (§3.4). |
| Pacer times the generated frame from measured frame times | AMD US 2025/0299287 (pending) | **Fixed back-to-back vsync presents** (§5): no frame-time estimator in the schedule. Without vsync (since 2026-10-03), a **fixed-rate clock** (refresh rate or a user constant), never a measured period. |
| Push-pull pyramid fill for holes *(round 2)* | Georgia Tech US 9,094,660 (granted, ~2033): reduce resolution until holes fall below a threshold, expand, fill from the expanded image | **Full-resolution fill** (§3.4): no mip chain of any kind, hole-aware or not. |
| CPU frame-start delay from measured GPU time *(round 2)* | Intel US 12,057,090 (granted): delay CPU work to align with GPU availability | **Dropped.** Latency reduction is the waitable swapchain's one-frame limit only (§5). |
| Store generated frames, then copy into the swap chain *(round 3)* | NVIDIA US 12,632,916 (granted 2026-05-19) and its continuation US 2026/0245168 (pending): in response to present calls, store real + interpolated frames in a first buffer, copy them to a swap-chain buffer, present | **The generated frame is composited straight into the back buffer** (§5); no intermediate frame queue, no API interception (the engine is the application). Counsel to confirm. |

Very broad NVIDIA applications have no clean design-around on their literal text; they are tracked, with
prior art on file (patents doc §6–7). US 2022/0038653 and 2022/0398751 (round 1); US 2024/0098216
(neural blending; **the held-back milestone 2**) and US 2025/0106355 (in-between frames "based at least in
part on depth", which reads on milestone 1 too; Yang 2011 is the prior art). US 2021/0067735 (any neural
network generating higher-frame-rate video) is reported **abandoned**. The hole fill works in colour space,
not by propagating motion, which keeps distance from 2022/0398751.

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
G4  fill       full-resolution colour fill for pixels with no valid candidate   procedural
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
- **Fill:** pixels where both confidences are near zero (true holes) take the **un-warped colour of
  frame N at the same pixel**, then 2–3 full-resolution passes of a 3×3 normalised convolution over the
  valid neighbours soften the seam. Everything runs at full scene resolution: **no pyramid, no mip chain,
  no reduce-then-expand step** (Georgia Tech US 9,094,660 claims exactly that structure; push-pull,
  Gortler et al. 1996, is its prior art, an argument for counsel only). Colour space only; motion is never
  propagated into holes. Holes are rare (both real frames failed), so a plain fill is enough. The fill
  must **not pick source pixels by depth similarity to the hole** ("same object" by a depth threshold):
  that is the amended claim of NVIDIA US 2025/0106355 (2026-07-16).

Milestone 1 is shippable on its own and is the baseline the network must beat. Estimate ~0.4–0.8 ms at
full 1440p, roughly a quarter at 0.5 scale.

### 3.5 Milestone 2 — the network: a learned trajectory prior (built 2026-10-03, opt-in)

**Built** (`--frame-interp-trajectory linear|quadratic|neural`, `--frame-interp-train`; default
**neural** since the engine ships trained weights). The quadratic path is `q = p − 0.5·v − 0.125·a` with
`a = v − v′` (v′ fetched backward at `p − v` with a depth check, else a = 0). `quadratic` uses that
analytic `a`; `neural` (**NeuraFI**) adds a 14→32×2→2 MLP's **correction** to it. Predicting the
correction, not the whole `a`, matters: a whole-`a` network could not match the analytic answer's
0.003–0.018 px on slow wide pans. The correction layer starts at zero, so a fresh network *is* the
quadratic and only learns improvements on it.

**Training** is self-supervised from ordinary real frames: interpolate the two-frame span N−2→N and take
N−1 as the answer (`target = (8·v_N − 4·v_span − (v_span − v′_span)) / s`), purely geometric; corrections
above half the span's motion are rejected as motion-vector seams (none seen so far). Weights save every
500 steps to `neurafi_v2.avnn` in the user data folder (lifetime steps and the gate's scores in
`.avnn.steps`); the user's file wins over the copy shipped in `bin/data`. Learning rate
`5e-4 / (1 + lifetime steps / 1000)`, floored at `2e-5`, over the network's whole life.

**The live gate.** Whenever Learned is chosen — trained or not — the same three-frame check records are
built from the user's own frames and the network is scored on the CPU twin every 100 frames (0.01 ms).
Smoothed scores (EMA 0.2; 3 checks to judge) decide: in only at 90% of the quadratic's error, out as soon as
it is worse. *Why live:* weights trained on fast jitter scored 0.262 px on slow wide pans the quadratic got
to 0.010 — a verdict from someone else's motion does not transfer, so every user's own motion decides.

**The acceleration image** is sized to at most ~262k texels (one per 2×2, 4×4 or 8×8 pixels): *measured*,
a fixed 2×2 at 3532×1987 put 1.75M records through the network for 3.98 ms; adaptive, 0.92 ms.

**Measured** (Sponza day and night, wobbling cameras; mean in-between position error on two-frame spans):
- The network trained across 6°/40, 3°/20, 20°/120 and 12°/70 wobbles reached 10k lifetime steps; on a
  wobble it had never seen (12°/70) its first check scored 0.020 px against the quadratic's 0.034, and
  the session ended at smoothed 0.008 vs 0.021.
- Earlier, whole-`a` network: 0.031 → 0.023 px vs quadratic 0.108 on its own training motion, but 26×
  worse than the quadratic on unseen slow pans before retraining.
- Two more unseen motions before shipping: big slow sweeps (25°/150), first check network 0.009 vs
  quadratic 0.018, ending 0.005 vs 0.006 (the quadratic is near-exact there); quick swings (8°/30), first
  check 0.217 vs 0.238, ending 0.050 vs 0.226 — about 4.5× better.
- Costs at 1766×994: interpolation 0.92–1.40 ms with network inference; training +0.52 ms while on; the
  live check 0.01 ms.

**Refinement on realistic motion** (`--cam-wander AMP SPEED`: yaw, pitch and position drifting on
non-repeating sums of sines, so translation parallax and changing acceleration are both present). Six
sessions, Sponza day and night, 15k → 30k lifetime steps; smoothed errors at session end:

| Wander | Quadratic | Network | Gate |
|---|---|---|---|
| 1.0 at 1× (slow) | 0.007 | 0.008 | quadratic (tie) |
| 0.6 at 2.5× | 0.048 | 0.043 | network, on and off |
| 1.5 at 0.5× (big, slow) | 0.005 | 0.007 | quadratic |
| 1.0 at 4× (fast) | 0.251 | **0.193** | network |
| 0.4 at 6× (small, jittery) | 0.372 | **0.245** | network |
| 1.2 at 1.5× (medium) | 0.054 | 0.071 | quadratic |
| **held out**, training off: 0.8 at 3× | 0.076 | **0.071** | network after the first judgement |

So the network helps most where motion turns quickly (25–35%), ties where the quadratic is already
near-exact, and loses on some medium-pace paths — where the gate keeps the quadratic. **The limit is its
inputs:** motion vectors alone cannot tell rotation from translation parallax, which depends on depth.
The next step for a real gain is a v3 record adding real-frame view depth (relative to the neighbours)
and screen position — allowed by R6, which forbids only in-between-time inputs — retrained from the start.

**Shipped weights.** The engine ships the network trained above (30,036 lifetime steps: seven wobbles,
then the six wander sessions, on Sponza day and night) as `modules/render.neurafi/data/neurafi_v2.avnn`, deployed to
`bin/data` and listed in both packaging allowlists (`data/**`). Its sidecar holds only the step count:
no verdict ships, so every user's live gate judges it on their own motion first. *Measured* as a fresh
user (no user file): loaded from `bin/data`, quadratic stood in, judged after three checks, switched in
at network 0.057 px vs quadratic 0.232. NeonDistrict (moving objects) is not in the training yet: it hits
a GPU fault on load that predates this work.

Not yet measured: the image-level gain on one-frame spans, scenes with moving objects (NeonDistrict hits a
GPU fault on load that predates this work; it is being chased separately).

**Settings:** Editor Preferences → "Frame interpolation path" (Straight lines / Quadratic / Learned
(NeuraFI)) and "Train NeuraFI while frame interpolation runs", with a live status line (steps, learning
rate, smoothed errors). They apply while editing and in Play; the CLI flags outrank them.

*The design below is what was built; it is kept as the record of why.*

**Redesigned after the claim review** (patents doc §6.3). The earlier "network outputs the blend weight"
plan reads closely on NVIDIA 17/949,153's allowed claim ("use one or more neural networks to blend two or
more intermediate video frames"), so the network no longer touches blending at all. It improves only
**where** each candidate is gathered.

- **Job (N1):** per pixel of each real frame, a 2-D **acceleration** a(p), so the G1 search follows a
  quadratic path (`q = p − 0.5·MV + 0.125·a`) instead of a straight line. Curved motion (turning cars,
  orbiting cameras, falling objects) is the largest error linear gathering leaves. (Sign convention as
  built: `q = p − 0.5·MV − 0.125·a`, `a = v − v′`.) Prior art: Xu et al.,
  *Quadratic Video Interpolation*, NeurIPS 2019; Liu et al. 2020; Chi et al. 2020.
- **Inputs:** real frames only — frame N's colour, depth and motion, and frame N−1's motion fetched by
  **backward lookup** at `p − MV_N(p)` with a depth check.
- **Shape:** a small conv net at 1/4 resolution, upsampled; fp32 (precision is not a claim element
  anywhere — R10). Runs on `Aver.Render.Neural` with conv forward kernels added.
- **Training (in-engine, check with the owner first):** supervised **only on geometric targets** — the true
  half-step positions from engine motion vectors at 2× rate. No colour or perceptual loss reaches the
  network. Needs frame determinism first.

**Design rules (binding for anyone implementing it; from the review, for counsel to confirm):**

| | Rule | Keeps clear of |
|---|---|---|
| R1 | No network output enters G2/G3 or any value that sets or scales a blend weight ("reliability", "confidence", "visibility", a gate) | NVIDIA 17/949,153 |
| R2 | No network outputs colour, a residual, a mask, a kernel or a visibility map; no learned multiplicative term | Arm GB 2620919 / US 2024/0029196; Super SloMo US 10,776,688 |
| R3 | Never forward-project N−1's motion into N; fetch it backward with a depth check. Where the check fails, acceleration is **0** (linear), never inferred from neighbours. Nothing anywhere estimates motion for a vector-less pixel from same-depth neighbours | NVIDIA US 12,574,521; 17/949,156 |
| R4 | Geometric supervision only; the network never learns shading or lighting motion | Intel US 2025/0225705 |
| R5 | One trajectory per pixel: never a linear and a quadratic candidate with a choice between them | NVIDIA US 2022/0038653 |
| R6 | No input describes the in-between time (no midpoint camera, depth, G-buffer or partial render) | Disney US 12,288,281; 17/949,156 |
| R7 | No optical flow anywhere; no global-motion similarity as a tie-break; no nearest-depth scatter | NVIDIA US 12,229,970; Arm US 2026/0030797; Intel '705 |
| R8 | (only if a restoration pass is ever added) its sole inputs are the generated image and its procedural confidence | 17/949,156 |
| R9 | Network inputs are down-sampled by plain strided/box filtering of linear RGB — no edge filter followed by luma conversion | NVIDIA US 12,524,850 |
| R10 | fp16 vs fp32 is not an element of any published Intel claim; precision is an engineering choice | (correction) |

Alternatives considered and ranked lower: N2 (learned motion-vector reliability feeding G2 — closest to
the blending claim, dropped), N3 (single-image residual restoration pass — clean if it never gains a mask,
but the weakest quality gain), N4 (learned constants with no network at runtime — cleanest against network
claims but not neural, and a learned table setting the blend weight raises an equivalence question).

**Open for counsel:** AMD US 2026/0094228 as published reads on *any* trained network stage in a rendering
pipeline (this one, and the neural radiance cache alike); prior art before 2024-09 is plentiful (DLSS 2.0,
Chaitanya 2017, Xiao 2020, ExtraNet 2021).

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
- **No frame queue in front of the swap chain:** each present pass composites its frame (generated or
  real) **directly into the acquired back buffer** and presents it. Neither frame is stored as a finished
  image and later copied into a swap-chain buffer, and no present call is intercepted. The history ring
  holds scene-resolution HDR inputs, not finished output frames. *Caveat:* the continuation US
  2026/0245168 was **broadened** on 2026-06-16 (interception and "outside the swap chain buffer" struck),
  so this mitigation is weaker than it looks; prior art is the main route (patents doc §6.1).
- **Fixed cadence, no measured timing:** with frame interpolation on, the generated frame and the real frame
  are queued as **consecutive vsync presents** (interval 1), generated first. The display's refresh is
  the clock. There is no frame-time estimator, no GPU or UI timing in the schedule, no pacer thread
  deciding when to present.
- **Frame cap** at half the refresh rate (the real-frame rate the cadence needs).
- **Without vsync (tearing), since 2026-10-03:** frame interpolation runs, on a **fixed present clock**
  (`D3D12Device::frameInterpWaitForTick`). Each image, generated then real, is presented with
  `ALLOW_TEARING` on the next tick of a clock running at a **constant** rate: the display's refresh rate
  (read from the display mode, `EnumDisplaySettingsW`), or the rate the user sets (Editor Preferences,
  `--frame-interp-clock`). That is an ordinary frame-rate cap. No frame time, GPU time or present statistic
  sets the rate or a tick. The render thread sleeps to the tick *after* submitting the GPU work, so the
  GPU is never held. A present later than one whole tick restarts the clock from "now". **This is the
  one place the schedule reads the clock at all; counsel should confirm that a fixed-rate limiter which
  re-anchors on a missed tick is not "display timing determined from rendering metrics"** (AMD US
  2025/0299287 claim 1; claim 15 covers metrics of the interpolated frame). Rejected alternative:
  timing each present from the measured frame period (FSR3-style pacing), which is the claimed method.
  It also delays no CPU work to align with GPU availability (Intel US 12,057,090): the sleep aligns to
  the fixed clock, never to a fence.
- **Variable refresh:** still not adapted to. The fixed clock simply runs; the display shows what arrives.
- **Below 30 fps base:** warning in the stats overlay (decision 3); the cadence stays the same (the real
  frame simply holds for more vblanks).
- **Latency reduction is separate:** the waitable object with latency 1, and nothing else. A CPU
  frame-start delay from measured GPU time is **dropped**: Intel US 12,057,090 (granted) claims delaying
  CPU work to align with GPU availability.
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
- **Neural Visualiser (Window > Neural Visualiser, 2026-10-03).** NeuraFI's gather also writes a
  display-ready visualisation image (`neurafi.hlsl` `vizColour`) while one is chosen. The editor draws it
  over the viewport after the tonemap, alpha-blended (`NeuraFiVizFeature`, `sandbox/shaders/neural_visualiser.hlsl`),
  on both presented images. Views:
  - **Sources**: orange = newer frame, blue = older, grey = both agree, magenta = hole.
  - **Confidence**: heat map; magenta = hole.
  - **Path bend**: `0.125·|a|` px, the curved path's offset from the straight line.
  - **Network share**: `0.125·|correction|` px; zero while the quadratic stands in.

  The full scale (px) and the opacity are adjustable. "Show interpolated frames only" makes the device
  put the generated image in both slots (`IDevice::setFrameInterpShowGeneratedOnly`). The visualisation
  is output only: nothing reads it back, and it has no effect on the generated frame.

  CLI flags for bounded runs: `--neurafi-view 0-4`, `--neurafi-generated-only`.
  *Measured* (NewSponza, 300 frames, vsync off): the overlay draws at 59 FPS shown / 29 real on the 60 Hz
  clock.

---

## 7. Patents

Full sweep, element mappings and design-arounds: [NEURAFI_PATENTS.md](NEURAFI_PATENTS.md)
(engineering mapping, **not legal advice**; a freedom-to-operate opinion is still required). This design
applies the changes in §2.3. Round 2 (2026-10-03) re-checked the round-1 HIGH items against this design
(all now LOW) and found the Georgia Tech fill and Intel pacing patents. Round 3 charted prior art against
the broad NVIDIA applications and checked their status and family (no Singapore, EP, KR or JP members).
The network was redesigned to stay off blending entirely (§3.5) and built on the owner's go-ahead; the
open counsel points (the two-start search, AMD US 2026/0094228) are noted, with no change needed now.

---

## 8. Stage plan

| Stage | Deliverable | Neural |
|---|---|---|
| 0 | Object motion vectors, sky motion — **done** (`4ee7e3ec`) | — |
| 0b | G-buffer reason for frame interpolation; scene-cut signal | — |
| 1 | **Built (D3D12):** history in the generator, present-pass split (two submissions per frame — D3D12 lets a command list write only the current back buffer), 3 swapchain images decoupled from frames in flight, fixed vsync cadence, HUD/editor-lines/ImGui drawn on both images, Editor Preference, `RENDER.FRAMEINTERP`, `--frame-interp 0\|1\|2`, scene cuts (resize, G-buffer reset, camera jump > 2.5 m or 30°). Not built: the waitable swapchain object, the <30 fps warning | — |
| 2 | **Built:** G1 gather (2 search starts per frame), G2 confidence, G3 blend, G4 two full-resolution fill passes; eye adaptation held on the generated image, bloom and local exposure recomputed on it | — |
| 3 | **Built, opt-in (owner go-ahead 2026-10-03):** trajectory-prior MLP (§3.5, rules R1–R10) with in-engine self-supervised training, lifetime learning-rate decay and an evidence gate; analytic quadratic as its fallback; Editor Preferences. Open: image-level measurement, a project key for the packaged runtime (CLI only there) | Yes |
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
- Gortler, Grzeszczuk, Szeliski, Cohen, *The Lumigraph*, SIGGRAPH 1996 (push-pull; cited as prior art
  against US 9,094,660, not used).
- AMD FidelityFX SDK v1.1.4 — reference only.
- Briedis et al. 2021/2023; Ha, Ahn, Yoon 2025; RIFE, IFRNet, FILM, EMA-VFI — ideas only.
- Microsoft DXGI docs: flip model, waitable swapchain.
- Patents: see [NEURAFI_PATENTS.md](NEURAFI_PATENTS.md).
- Aver source: `voxi.hlsl`, `VoxiRenderer.cpp/.hpp`, `voxi_rt.hlsli` (stage 0); `D3D12Device.cpp:51,
  3882-3888, 3949, 5039-5491, 5842-5974`; `UiRenderer.cpp:170-237`; `EditorLines.cpp:392`.
