# NeuRaC — Neural Radiance Cache (Aver) — design

NeuRaC is the engine's world-space radiance cache for ReSTIR's second bounce (`--restir-visibility
cached`; code `NeuRaC.hpp/.cpp`, `NeuRaCLayout.hpp`, `voxi_neurac*.hlsl*`). Stage 1 is procedural (SH
cells); the neural part arrives in stage 2.

**Status:** stage 1 is **built, committed (`6057ff50`) and run** (2026-10-03). *Measured* on NewSponza:
the cache fills (cascade 0: ~18.8k valid cells, n_eff ~14 after 150 frames), the resolve costs
0.02–0.03 ms and the whole mode ~0.23 ms over HalfResolution; it supplies about half of the term it
replaces, the HalfResolution fallback the rest. Stage 2 and 3 are still design only. Where the
implementation departs from the design below, section 5.10 lists the deviation and the reason; the
older text in 5.1-5.9 is kept as the design of record and carries a pointer where it no longer matches.
Every millisecond and cell count below is an estimate unless it says *measured*.
Written 2026-10-03 from three rounds of read-only scouting (algorithm, hardware, prior art, and the
Aver source).

**One-line summary:** a camera-centred cascade of cells around the viewer, each holding first-order
spherical harmonics (SH) of incident light, trained live from the rays ReSTIR GI already traces, and
read in place of the second-bounce ray on the pixels that do not trace one. Neural pieces arrive in
stage 2 on top of it, through a new shared module, `Aver.Render.Neural`.

---

## 1. Goals and non-goals

**Goals**

- A world-space radiance cache that runs on **any D3D12 GPU** (AMD, NVIDIA, Intel), with no vendor
  SDK, no CUDA, no Agility SDK, at the engine's current shader-model ceiling (SM 6.5/6.6).
- Built **in-house from scratch**, so it can ship under the engine's LGPL-2.1 licence.
- A **shared foundation** (`Aver.Render.Neural`) for later neural features — upscaling and frame
  interpolation reuse its optimiser, kernels and sample plumbing.
- First use: make ReSTIR GI's second bounce cheaper and better than today's options.
- Off means off: with the cache disabled, existing entry points compile from the same shader text and
  defines, and no new resource or pipeline is created.

**Non-goals (for now)**

- Matching NVIDIA NRC's per-pixel network. On the dev GPU (RX 7800 XT, RDNA3) there is no D3D12 route
  to matrix hardware, and the paper's 5×64 network is estimated at 8–18 ms there in plain HLSL.
- Glossy/specular indirect. Today's second bounce is diffuse-only (`voxi_restir.hlsli:792`,
  `radiance += s.kdAlbedo * indY`); glossy queries are a new capability, planned for stage 3.
- Cache-guided ReSTIR sampling. Parked: it sits in patent territory (see §9).
- Vulkan. D3D12 first, Vulkan parity afterwards, as for every other feature.

---

## 2. Constraints that shaped the design

| Constraint | Consequence |
|---|---|
| RX 7800 XT has no D3D12 matrix path. Cooperative Vectors never covered RDNA3 and are deprecated; LinAlg (SM 6.10) is preview and RDNA4-only; WaveMMA was removed from DXC; AGS exposes no WMMA. RDNA3 WMMA is reachable only through Vulkan `VK_KHR_cooperative_matrix`. | Portable HLSL is the baseline backend for every vendor. Matrix backends (Vulkan coop-matrix, later D3D12 LinAlg) are optional extras behind one interface. |
| Network maths is cheap at cache scale. About 100k decodes of a 2×32 MLP is ~0.02 ms of ideal FMA time (derived). | fp32 first. Packed fp16 later, gated on a new caps query. **No int8**: it needs per-step requantisation, HLSL has no mixed-sign `dot4`, and it is no faster than packed fp16 on AMD or NVIDIA. |
| The real cost is memory traffic: cell reads/writes, scattered atomics, weight loads. | Keep payloads small (24 B/cell), keep work per *cell* rather than per *pixel*, and accumulate with integer atomics. |
| D3D12 has no fp16 atomic add. Only u32 add/CAS is portable; fp32 atomics are optional. | Training accumulates in **u32 fixed point**, like the existing voxel volume (`voxi.hlsl:182, 4355-4357`). Integer sums are also order-independent, so training is deterministic. |
| Activision's *Neural Light Grid* (SIGGRAPH 2024) found that a learned latent per cell plus one shared small decoder **failed**; it needed 512–1024-wide decoders (3–10 ms per brick). What shipped was ordinary probes plus tiny 2×8 networks that learn the blend weights. | No "latent grid + shared tiny decoder" on the main path. Neural work starts with learned blend weights and spatially local networks. |
| Aver's voxel GI is one **world-fixed** cube over the level (`GameLevel.cpp:604-614`). On NeonDistrict that is ~8.6 m cells even at 512³. | The cache needs its own camera-local cascades and its own geometry source; it cannot reuse the voxel volume's cells or rebuild gate. |
| Voxi's binding table 0 was full: 22 SRV and 20 UAV, hard-asserted (`VoxiRenderer.cpp:296`). | The cache gets its own small binding set for the resolve pass. Voxi's table was widened deliberately, with the static_assert, to 23 SRV / 22 UAV (t22, u20, u21; see 5.10 item 4). |

---

## 3. Architecture: the layers

```
 L0  analytic base        sun + sky + existing voxel GI            (exists)
 L1  cell cache           camera-centred cascades of SH cells      (stage 1, no network)
 L2  learned corrections  tiny blend-weight nets, local residual    (stage 2, neural)
     nets
 L3  per-pixel read       SH(N) x albedo in place of the 2nd bounce (stage 1)
     glossy query         direct network query for smooth/metal     (stage 3, neural)
```

The stack works because each layer only carries what the one below cannot:

- L0 is free and always available: it is the cold-start answer and the fallback.
- L1 stores what L0 gets wrong at the camera's scale. L0's cells are metres wide; L1's are 0.25 m near
  the camera.
- L2 learns only the residual error of L1 (leaks, detail SH cannot hold), so its networks stay tiny.
  An untrained L2 outputs ~0, which leaves L1 unchanged.

---

## 4. Stage plan

| Stage | Deliverable | Neural | Exit criterion |
|---|---|---|---|
| 0 | Measurements: occupied-cell counter; GPU cost of today's F2 trace and voxel tap (`--gpu-timing`). The owner runs these. | — | Numbers in this doc replace the estimates. |
| 1 | L1 cell cache + L3 read as a new ReSTIR visibility mode. Infrastructure: cascades, sample-driven occupancy (no voxeliser, see 5.10), sample stream, fixed-point scatter, resolve, bindings, settings. **Implemented, pending verification.** | No | Beats `HalfResolution`'s sky-ratio reconstruction against the path tracer at ≤ its cost, on PTTest and NeonDistrict. |
| 2a | `Aver.Render.Neural` MLP kernels + Adam optimiser + CPU reference; first client: learned leak/blend weights (Activision-style, 2×8). | Yes | Visible leak reduction at thin walls; no regression elsewhere. |
| 2b | KiloNeRF-style local residual networks per region, correcting L1's SH. | Yes | Measurable error drop vs the path tracer for ≤ 0.5 ms. |
| 3 | Glossy/metal direct query (fixes the metallic "black hole": `kdAlbedo = 0` hits get no indirect today). | Yes | Metals and roughness < ~0.45 receive indirect light. |
| — | Shared decoder over learned latents (old idea). | Yes | Research only: has a published failure (§2). |

---

## 5. Stage 1 in detail — the cell cache

### 5.1 Cascades

- **Layout (open decision D2):** 3 cascades of 64³ cells, at 0.25 m / 1 m / 4 m cell size. They cover
  16 m / 64 m / 256 m cubes centred on the camera. Beyond 256 m the read falls back to L0.
- **Scrolling (toroidal) addressing:** a cell's texel is `worldCell mod 64` per axis. When the camera
  moves, only the newly exposed slabs are cleared and re-seeded; nothing is copied.
- **Cell count:** 3 × 64³ = 786,432 cells. A surface-area estimate puts ~5–14% occupied, about
  37k–111k occupied cells in total (*derived; stage 0 measures it*).

### 5.2 Geometry source (occupancy)

> **As implemented:** there is no voxeliser. Occupancy is *sample-driven*: a cell exists because a
> traced F2 ray started inside it (section 5.10, deviation 1). The text below is the original design.

The world-fixed voxel volume is too coarse at cascade scale, so the cache builds its own occupancy:

- a cheap conservative voxelise of the static and movable draws into each cascade's **occupancy
  bitmask** (1 bit per cell, 32 KB per cascade). Only exposed slabs are re-voxelised when the camera
  scrolls; dirty regions are re-voxelised when the existing scene-change gate fires.
- an optional **distance-to-surface byte** per cell (a few jump-flood steps over the bitmask), used
  for leak control (§5.6).

Movers and skinned meshes are absent from the existing voxel volume. The cache's voxeliser should
include movers from the start, so lighting near them is not frozen.

### 5.3 Cell payload

> **As implemented:** cells are 32-byte structured-buffer elements, not `RGBA16F` 3D textures, and
> the accumulator is 15 ints per cell plus a separate counts region (section 5.10, deviations 4-5).
> The text below is the original design.

- **Incident radiance as first-order SH, RGB:** 4 coefficients × 3 channels = 12 values, stored as
  fp16 in three `RGBA16F` 3D textures per cascade (24 B/cell; ~6 MB per cascade, ~19 MB total).
- Hardware trilinear filtering is valid on SH coefficients, which is why SH was chosen over spherical
  Gaussians (not linearly filterable) or per-cell octahedral maps (too large for dense cells).
- At lookup, irradiance is the cosine-convolved SH, with ZH3-style sharpening (Roughton 2024) as a
  cheap upgrade on the diffuse term.
- An **accumulator** per cascade: 12 signed fixed-point sums + 1 sample count = 13 × u32 per cell. It is
  written only during the frame and cleared by the resolve.

### 5.4 Training samples — where they come from

ReSTIR GI already traces a second-bounce ray on a subset of pixels: F2 path 3 (`voxi_restir.hlsli:751-791`).

- That ray starts at the GI candidate's hit point `hitPos` with normal `s.N`, goes in direction `dir2`,
  and returns `indY`: incident radiance from `dir2` (sky on a miss, the voxel volume on a hit).
- Each such ray is **one Monte Carlo sample of incident radiance at `hitPos`**. It is projected onto
  the 4 SH basis functions in direction `dir2`, weighted by its sampling pdf, and scattered into the
  accumulator cell containing `hitPos` (fixed-point `InterlockedAdd`, 13 atomics per sample).
- Sample volume: F2 traces on every traced pixel under `Full` and on about ¼ under `HalfResolution`,
  i.e. ~0.35–1.4M samples per frame at 1440p (*derived*). That is ~10–40 samples per occupied cell per
  frame.

Two properties matter here:

- **No new rays.** Training rides on rays the frame already pays for.
- **No self-reference in stage 1.** A sample's value comes from the sky or the voxel volume, never from
  the cache itself. That keeps the cache single-bounce-over-L0 for now. Feeding the cache back into its
  own samples (multi-bounce) is possible later, but it is a deliberate design and patent decision (§9),
  not a default.

**Open decision D1:** the sample does not need the hit's albedo, because it is *incident* radiance at a
point. Widening `RdGiCand` (`voxi_restir.hlsli:129`, 48 B) to carry albedo and roughness is needed only
for stage 3's glossy queries. Recommendation: defer the widening until stage 3.

### 5.5 Resolve (once per frame per cascade)

For each cell with samples this frame:

- the new SH is the sample mean (sums ÷ count);
- it is blended into the stored SH with a temporal factor `α = max(1/(n_eff+1), α_min)`, where
  `n_eff` counts frames the cell has been observed. That gives fast convergence on a fresh cell and a
  floor for responsiveness when lighting changes;
- the accumulator is cleared.

Cells with no samples keep their value and age. Cells older than a threshold fade towards L0.

Stage 1 needs no gradient optimiser: SH projection is linear, so the running mean is already the
least-squares fit. Adam arrives with stage 2's networks, in the shared module.

### 5.6 Read (L3) and leak control

The cache replaces the second bounce on **pixels that do not trace one**. Today those pixels use a
voxel-cone march (`Reconstructed`, f2Path 1) or a sky-luminance ratio (`HalfResolution`'s untraced
pixels, f2Path 2). A new f2Path reads:

```
indY = irradianceFromSH(cacheSample(hitPos + s.N * offset), s.N)
```

- **Cascade selection:** the finest cascade containing `hitPos` with ≥ 1 voxel of margin, cross-faded
  over the last 4 cells to hide seams.
- **Normal offset:** look up half a cell along `s.N`, the same idea as the existing shell-straddle fix
  (`voxi_restir.hlsli:763-775`).
- **Occupancy-weighted trilinear:** manual 8-tap with weights zeroed where the occupancy bitmask says
  the corner cell is inside solid geometry or behind the surface. This is what stops light leaking
  through thin walls, which plain hardware trilinear cannot do.
- **Confidence fallback:** a cell's age and sample count give a confidence that blends towards L0 (the
  existing voxel/sky answer) while the cache is cold.

Integration is a small, local edit: one more branch in the `indY` block at
`voxi_restir.hlsli:725-791`, plus a new `RestirVisibility` value (Voxi.hpp:293). A working name is
`Cached = 4`: traced pixels train the cache, untraced pixels read it.

### 5.7 Frame placement

> **As implemented:** there is no occupancy-voxelise step in the frame, and the scatter and the read
> run only inside the four staged-compute *twin* pipelines (section 5.10, deviations 3 and 7).

```
voxelize -> filterMips -> airVis                      (existing L0)
cache: scroll/clear slabs -> voxelise occupancy        (new, small)
CSRdVisibility -> CSRdGiTrace (F2 samples -> accumulate) -> CSRdShadow -> CSRdGi -> ...
cache: resolve (accumulator -> SH)                     (new; used by NEXT frame's reads)
```

The resolve sits after the trace, so reads in frame N use the cache resolved in frame N−1. One frame
of latency on a cache that is already temporally smoothed is acceptable, and it avoids a mid-frame
dependency between trace and read.

### 5.8 Bindings, settings, determinism

> **As implemented:** Voxi's table 0 gains three slots, not one SRV (t22, u20, u21), and the wire
> encoding is mode 2 plus bit 128, not a fifth mode value (section 5.10, deviations 2-3). The "off
> means off" contract holds in the narrow form stated below. No cascade-size or cell-size settings
> keys exist yet: cascade count, resolution and cell sizes are compile-time constants.

- **Bindings:** the cache's own compute layout for its passes (accumulator UAVs, SH UAVs, occupancy),
  modelled on the voxel mip/resolve sets. Voxi's table 0 gains one SRV for the resolved SH, widening
  `kVoxiSrvCount` and the static_assert together.
- **Settings:** a new `RestirVisibility::Cached` value plus cascade size and cell-size keys, resolved in
  `RenderSettingsResolver` with a `DisableReason` when ray queries or the GI path are unavailable.
  Default stays `HalfResolution`; it is not on the quality ladder until measured.
- **Determinism:** integer accumulation makes training order-independent. The "off" contract is
  stated narrowly: same shader text and defines for existing entry points, no new resources or
  pipelines. "On" is judged statistically against the path tracer, not by bit equality, because
  ray-traced gate configs already do not reproduce run to run.

### 5.9 Cost estimate (stage 1, 1440p, RX 7800 XT) — all estimates

| Part | Estimate |
|---|---|
| Sample scatter: 0.35–1.4M samples × 13 u32 atomics | 0.1–0.4 ms (contention-dependent) |
| Resolve: ~100k touched cells | < 0.1 ms |
| Occupancy voxelise (scrolled slabs only) | < 0.1 ms typical; spikes on teleports |
| Read: 8-tap occupancy-weighted SH lookup per untraced pixel | 0.1–0.3 ms |
| **Saved:** cone march or sky-ratio replaced; with `Cached` at the HalfResolution trace rate, quality should approach `Full` without its extra rays | to be measured in stage 0 |

### 5.10 As implemented: deviations from the design

Everything in 5.1-5.9 stands unless listed here. Each item names why the design could not be built as
worded; none was a free choice. All of this is **implemented-pending-verification**: no stage-1 code
has been built, run or measured at the time of writing.

1. **Sample-driven occupancy replaces the camera-local voxeliser (5.2, D4).** A cell is created by the
   F2 samples that land in it, and the lookup trusts only cells whose tag, `n_eff` and age say a
   sample put them there. Consequences: no occupancy bitmask, no distance-to-surface byte, no
   exposed-slab re-voxelise on scroll or teleport (a window that shifts makes every tag mismatch, which
   is the reset), and **movers and skinned meshes need no special case** because the samples come
   from the same rays that already see them. The cost is that leak control is the mean-normal weighting
   described under item 6 rather than an occupancy test; thin walls can still leak where one cell holds
   two opposed surfaces.
2. **Wire encoding: mode 2 plus bit 128, not a new mode value (5.8, D3).** The shader decodes the mode
   as `(uint)gAmbientParams.w & 3u` (and `givis::packAmbientW` masks the same way), so a 4 would
   silently become 0 (NoRay), the over-bright pre-fix look. The CPU keeps `RestirVisibility::Cached = 4`
   in `Settings` and in the renderer, and packs mode 2 (`HalfResolution`) plus the new `neurac`
   bit, value 128 (`packAmbientW`'s ninth argument, default false). The existing
   `halfBound`/`tracedPx`/`rec.valid` logic then makes the tracing decisions exactly as for
   `HalfResolution`, and the half-resolution history pair is still allocated (`giVisHistWanted` accepts
   2 and 4). 128 is the last free bit below 2^23 inside the already-exact low byte; the word is a float
   and must stay under 2^24. Typos above 4 clamp to 3 (Full), never to Cached.
3. **Four lazily compiled twin pipelines, not runtime gating (5.8 "off means off").** Adding scatter and
   lookup code to `voxi_restir.hlsli` unconditionally would change the text of every variant that
   includes it, including single-pass `PSRayDriven`, which is documented at the AMD register limit. All
   new HLSL therefore sits under `#if AVER_NEURAC` (default 0, with a hard `#error` against
   `AVER_RD_SINGLE_PASS`), and the define is set only on four staged compute twins built on first use:
   `CSRdGi`, `CSRdGi` + checkerboard, `CSRdGiTrace`, `CSRdGiTrace` + checkerboard. Each frame the
   renderer picks a twin only when bit 128 is set and the twin exists. With the cache off, the existing
   variants compile from identical preprocessed text; the DXIL cache still invalidates once, because
   its key hashes the whole shader corpus.
4. **Cells are read through a UAV, not an SRV (5.8).** Slots: `t22` cascade info (CPU-written Upload
   ring of 3, fixed GENERIC_READ), `u20` accumulator, `u21` cells. An SRV for the cells would need
   UAV-to-SRV buffer barriers around two different reader states inside the deliberately barrier-free
   staged lighting group, and would take the last of the 24 table slots. UAV-only buffers need no state
   transitions at all. Table 0 goes from 22 SRV / 20 UAV to 23 SRV / 22 UAV, and the static_assert in
   `VoxiRenderer.cpp` (and the text `GiVisibilityTest` greps for) says so. Nothing is bound at init:
   the new slots are null-filled until the cache is first needed, and a tiny placeholder buffer is
   created lazily at teardown only, because a UAV slot cannot be cleared once set.
5. **32-byte cell packing and a 15-int accumulator (5.3).** 12 SH values as fp16 are 24 bytes, the
   octahedral mean normal is 4, which leaves 4 for the rest: a **24-bit tag** (3 x 8 bits of
   `(worldCell >> 6) & 255` per axis), 4-bit `n_eff` and 4-bit age. The accumulator is 12 signed
   fixed-point SH sums plus 3 summed-normal sums per cell (scales `2^15` and `2^16`; the headroom is a
   `static_assert` in `NeuRaCLayout.hpp`), with the per-cell counts in their own contiguous
   region so the resolve can early-out by streaming 3 MB instead of touching 786k sparse cache lines.
   The tag aliases every 256 x 64 cells (about 4 km at the finest cascade) and is bounded by age expiry;
   a 48-byte cell is the fallback if it ever matters.
6. **Lookup details (5.6).** Per cascade: normal-offset by half a cell, 8-corner manual trilinear
   with each corner weighted by its trilinear weight, `saturate(dot(cellDir, N) * 2)`, the mean-normal
   length (planarity), `min(n_eff / 4, 1)` and `1 - age / 15`; cascades blend finest-first with a
   4-cell edge fade and a 1-cell margin; the remainder falls back to the f2Path 2 formula (the L0
   answer). The SH is L1 with the Ramamoorthi-Hanrahan cosine convolution; ZH3 sharpening is still
   a TODO.
7. **Staged ray-driven on D3D12 only.** The scatter and the read live in `giTraceInitialCandidate`'s
   F2 block, reachable from the staged `CSRdGi` and `CSRdGiTrace` twins. Single-pass `PSRayDriven`
   (register limit) and the raster `PSMainVoxi` (not worth a multiplicative variant set) never compile
   the cache, and the staged passes themselves are D3D12-only (Vulkan cannot record compute inside the
   scene pass). **Everywhere else `Cached` behaves exactly as `HalfResolution`**: the resolver reports a
   soft `DisableReason::RequiresStagedRayDriven` (it warns in the UI, never greys the control, and
   `effective` still equals `requested`), and the renderer logs the fallback once. The split `CSRdGi`
   variant (`AVER_GI_SPLIT`) only reads the candidate buffer and never traces, so it needs no twin.
8. **VRAM is about 75 MB, not 19 MB (5.3, 5.1).** Accumulator 50.3 MB (12,582,912 ints) plus cells
   25.2 MB (786,432 x 32 B), about four times the original estimate, because 15 ints per cell are
   accumulated rather than 13 and no packed texture form is used for the sums. The owner has accepted
   this. If it proves too much: 48^3 cascades, or a 16-bit accumulator; neither is free.
9. **Zero-init and reset are one mechanism.** The RHI has no buffer clear, so the resolve shader has a
   `CLEAR_ALL` flag (bit 0 of the info word) that zeroes every cell and the whole accumulator; creation
   and `requestReset()` record one such dispatch. Teleports need no reset (item 1).
10. **Settings surface.** `RestirVisibility::Cached = 4` is selectable in the Project Settings combo,
    the `voxi.giRestirVisibility` console variable (0-4), `--restir-visibility cached` and the
    `RENDER.RESTIRVISIBILITY` project key (4). No quality-ladder rung returns it: it is opt-in until
    measured. The design's "cascade size and cell-size keys" are deferred; the occupied-cell counter of
    stage 0 is deferred too (a readback needs fence-synchronised `readBuffer`).
11. **Timing and debug.** GPU stats: `"Voxi NeuRaC resolve"` and `"Voxi NeuRaC clear"`.
    Scatter and lookup cost lives inside the existing RD lighting spans, so cache versus plain is an
    A/B of mode 2 against mode 4 on those. The F2 path view (`voxi.giVisPathView`) paints f2Path 4 magenta,
    brightness = cache confidence (not in the split `CSRdGi`, which has no twin).
12. **Neural Visualiser (Window > Neural Visualiser, 2026-10-03).** Shows the cache at the visible
    surfaces in place of the lit image (`rcDebugColour`, `voxi_neurac_io.hlsli`). The twin
    `giRestirIndirect` returns the colour after the denoiser-input write (so it never enters history),
    and Stage B shows it unshaded. Views:
    - **Cached light**: `rcLookup` alone; black = nothing cached.
    - **Coverage**: red = fallback, green = cache.
    - **Cascade**: cyan 25, yellow 100, orange 400 cm cells.
    - **Cell state**: green = n_eff, red = age, violet = empty or stale.

    An optional **cell grid** darkens cell edges along the surface. The mode travels in `gAmbientParams.w`
    bit positions 8-11 (`givis::packAmbientW`'s `neuracView`), set only beside the live-cache bit.

    While a view is up the GI split stands down for the frame, because only the unsplit `CSRdGi` has a
    cache twin. CLI flags: `--neurac-view 0-4`, `--neurac-grid`.

    *Seen* on NewSponza (200 frames, Cached): the cascades are correct (cyan near, yellow beyond, grid
    aligned). Coverage after 200 frames is green on floors and red on most vertical surfaces (curtains,
    pillars, walls), so the cache mostly serves upward-facing surfaces so far. Worth a look against the
    warm-up stats (cascade 0 holds 7% of cells valid at frame 150).

**What is unverified (all of it):** that the twins compile and fit in registers on the RX 7800 XT
(a `--dred` or crash check is needed, since `CSRdGiTrace` is a monolith with ray queries), the atomic
cost of roughly one million samples x 16 atomics per frame, the resolve cost, the image quality against
the path tracer, and Vulkan, which is untouched and needs its own parity pass.

---

## 6. `Aver.Render.Neural` — the shared foundation (stage 2a)

An RHI-only static module like `Aver.Render.Denoise`: links `Aver.Core` + `Aver.RHI`, owns its shaders
(deployed with `aver_deploy_shaders`), records into a caller's `IRenderContext`, and never sees Voxi
types. Sketch, not final:

```cpp
namespace aver::render::neural {

// What the device can run. Filled from DeviceCaps (+ a new OPTIONS4 native-16-bit query).
enum class Backend : u32 { PortableFp32 = 0, PortableFp16 = 1, VulkanCoopMatrix = 2, D3D12LinAlg = 3 };

struct MlpDesc {
    u32 inputs, outputs;
    u32 hiddenWidth, hiddenLayers;      // e.g. 8x2 (blend weights), 32x2 (local residual)
    Activation hidden = Activation::ReLU, output = Activation::None;
    u32 instances = 1;                  // >1 = many small networks (one per region, KiloNeRF-style)
};

struct OptimiserDesc {                  // Adam; fixed-point gradient accumulation, then a step pass
    f32 learningRate, beta1 = 0.9f, beta2 = 0.99f, epsilon = 1e-8f;
    f32 weightEma = 0.99f;              // queries read the smoothed copy (stability)
    f32 gradFixedScale;                 // fixed-point scale for u32 gradient atomics
};

class Network {
public:
    bool create(rhi::IDevice&, const MlpDesc&, const OptimiserDesc&);
    // Inference: records -> outputs. Records are a caller-owned structured buffer.
    void recordInfer(rhi::IRenderContext&, rhi::BufferHandle records, u32 count, rhi::BufferHandle out);
    // Training: records + targets -> gradient accumulation -> one optimiser step.
    void recordTrain(rhi::IRenderContext&, rhi::BufferHandle records, rhi::BufferHandle targets, u32 count);
    Backend backend() const;
};

// CPU reference of the same maths, for tests (no GPU needed).
std::vector<f32> referenceInfer(const MlpDesc&, std::span<const f32> weights, std::span<const f32> input);

}  // namespace aver::render::neural
```

**Kernel rules:**

- one thread per record;
- weights loaded cooperatively into groupshared per layer (layer-streaming, not one fused
  register-resident kernel; see §9);
- `[numthreads]` a multiple of 64, and no assumptions about wave size, so AMD wave32/64, NVIDIA 32 and
  Intel 8–32 all behave the same;
- fp32 by default, with an fp16 variant compiled only when the device reports native 16-bit ops.

**Engine changes it needs:**

- an `OPTIONS4` (`Native16BitShaderOpsSupported`) query in `queryCaps`;
- SM 6.4 and 6.2 added to the probed shader-model list (`D3D12Device.cpp:2700`), so a 6.4 device is not
  recorded as 6.1.

No Agility SDK.

---

## 7. Hardware backends

| Backend | Runs on | Status |
|---|---|---|
| Portable fp32 HLSL | every D3D12 GPU | stage 2a baseline |
| Portable fp16 HLSL | devices with native 16-bit ops | after the caps query lands |
| Vulkan cooperative matrix | AMD RDNA3+ (WMMA), NVIDIA (tensor cores) | after Vulkan parity; RDNA3 WMMA is ~1.5–3× plain fp16 at best |
| D3D12 LinAlg (SM 6.10) | RDNA4, NVIDIA, Intel | when retail; dev-only (Developer Mode) until then |

Weights are trained on the user's machine and never shipped, so each backend may use its own layout
and precision.

---

## 8. Measurements needed (stage 0, owner runs)

1. Occupied-cell fraction per cascade on PTTest and NeonDistrict (needs a small counter pass).
2. Cost of today's F2 trace (f2Path 3) and the cone march (f2Path 1) via `--gpu-timing`.
3. After stage 1: cache vs `HalfResolution` vs `Full` against the path tracer, on the same views.

---

## 9. Patents and licences (flags, not legal advice)

- **US 11,610,360** (NVIDIA, neural radiance caching). Independent claim 1 requires a *neural network*
  radiance cache queried at a 3D position, an image made from its prediction, and weights updated from
  a second prediction at the end of an extended path. Stage 1 has no network and no self-referential
  training. Stage 2 networks learn blend weights and residuals over a non-neural cache. Counsel should
  review stage 2 before it ships, and any later multi-bounce feedback.
- **US 11,631,210 / 11,935,179** (NVIDIA, fully fused execution): thread-block partitions, weights
  loaded once into the register file, intermediates in shared memory, layer by layer (and in '179,
  weight blocks sized to a hardware matrix unit). The module's kernels use per-layer groupshared
  streaming. Whether that is different enough is counsel's call.
- **US 11,315,310 / 12,299,801** (ReSTIR GI, ReGIR): why cache-guided ReSTIR sampling is parked, and why
  `voxi_reservoir.hlsli:20-22` already forbids per-cell light reservoirs.
- **Code:** nothing is vendored. NVIDIA NRC/RTXGI/RTXNS (proprietary), instant-ngp (non-commercial)
  and VkNRC (no licence) are references only. tiny-cuda-nn (BSD-3, CUDA) and AMD MiniDXNN (MIT) are
  readable references.

---

## 10. Open decisions

| # | Decision | Recommendation |
|---|---|---|
| D1 | Widen `RdGiCand` with albedo/roughness now, or train on incident radiance only | Incident radiance now; widen in stage 3 |
| D2 | Cascade layout | 3 × 64³ at 0.25 / 1 / 4 m; fall back to L0 beyond 256 m. *Implemented as designed.* |
| D3 | First integration | New `RestirVisibility::Cached`: traced pixels train, untraced pixels read. *Implemented, with wire mode 2 + bit 128 (5.10).* |
| D4 | Movers in the cache voxeliser | Yes, from the start. *Moot: there is no voxeliser; sample-driven occupancy covers movers (5.10).* |
| D5 | Time budget on the RX 7800 XT at 1440p | Stage 1 ≤ ~0.6 ms; stage 2 ≤ ~0.5 ms more |

---

## 11. Sources

- Müller et al., *Real-time Neural Radiance Caching for Path Tracing*, 2021 — arXiv 2106.12372.
- Iwanicki, Sloan, Silvennoinen, Shirley (Activision), *Neural Light Grid*, SIGGRAPH 2024 Advances.
- Reiser et al., *KiloNeRF*, ICCV 2021. Fridovich-Keil et al., *Plenoxels*, CVPR 2022.
- Roughton, *ZH3: Quadratic Zonal Harmonics*, 2024. Ramamoorthi & Hanrahan, 2001 (irradiance and SH).
- Microsoft: Shader Model 6.4 (`dot2add`, `dot4add_*`), 6.9 retail notes, LinAlg (SM 6.10) preview.
- AMD GPUOpen: WMMA on RDNA3. Chips and Cheese: RDNA3 microbenchmarks.
- Google Patents: US 11,610,360; US 11,631,210; US 11,935,179.
- Aver source: `modules/render.voxi/shaders/voxi_restir.hlsli:129, 725-792`; `voxi.hlsl:182, 3671,
  4355-4385`; `VoxiRenderer.cpp:183-198, 296`; `Voxi.hpp:279-294`; `D3D12Device.cpp:268-284, 2700`.
