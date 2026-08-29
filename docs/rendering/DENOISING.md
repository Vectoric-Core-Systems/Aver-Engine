# Denoising, and why NVIDIA NRD is not the answer here

**Investigated 2026-08-27.** The question was whether to vendor NVIDIA's Real-time Denoisers (NRD) to
replace the hand-written spatial/temporal filters in `modules/render.voxi`.

**The answer is no, and the licence is the least interesting reason.**

---

## 1. What we have today

| | |
|---|---|
| Spatial shadow filter | `rtShadowSpatial`, `VoxiShaders.hpp`. Square gather over the shadow history, plane-distance accept/reject. |
| Spatial reflection filter | Added 2026-08-27 (`rtReflectionSpatial`). There was none before. |
| Temporal reprojection | `rtReprojectHistory` / `rtReprojectReflection`. Current world position through **last frame's** view-projection. |
| Ray-tile amortisation | `rtPixelsPerRayTile`. |

Two of those are switched off in everything we ship, which is worth stating plainly because it is
easy to look at the code and conclude otherwise:

- **`rtShadowDenoiseForQuality` returns 0 at every tier, Epic included** (`Voxi.cpp`). Radius 0 hits
  an early return, so the spatial shadow filter is a **permanent no-op in the shipped product** — the
  loop, its cost and its weighting are all implemented and none of them execute.
- **`rtPixelsPerRayTileForQuality` returns 1 at every tier**, which forces `tileBits = 0`, so the
  "reuse last frame's ray" path is dead too. Every pixel traces a fresh shadow ray every frame.

So the honest description of the shipped denoiser, before this session, was: **a temporal
reprojection for shadows, nothing at all for reflections, and a spatial filter that never runs.**

## 2. What NRD would need, and what this renderer has

NRD resolves noise using per-pixel G-buffer guides. Its three required inputs are:

| NRD input | What it is | Does Aver have it? |
|---|---|---|
| `IN_MV` | Motion vectors; NVIDIA recommends 2.5D or 3D over 2D | **No.** Nothing in this engine produces motion vectors. |
| `IN_VIEWZ` | Linearised view-space depth of primary hits | **No** — as a texture. Depth exists in the depth buffer; nothing resolves it for a compute pass. |
| `IN_NORMAL_ROUGHNESS` | World normal + linear roughness + material ID, packed | **No.** Both exist only in pixel-shader registers. |

That is not a near miss. **Aver is a forward renderer**: `PSMainVoxi` returns a single `SV_TARGET`,
and normal, roughness and albedo are local values that never reach a texture. The complete set of
full-screen textures the Voxi shaders declare is the voxel volume, the shadow atlas, the GI-only
shadow map, and the two ping-ponged RT history pairs. There is no G-buffer to guide anything with.

The declaration `UpscalerNeeds::MotionVectors` exists in `RHIResources.hpp`, and its own comment
already says the truth: nothing in this engine produces them.

Two consequences follow, and the second is the one that matters:

- The existing reprojection is **static-geometry only**. It transforms *this* frame's world position
  through *last* frame's camera, which is correct for a world point that did not move and silently
  wrong for one that did. `RtInstance` carries only a current-frame `objectToWorld`; there is no
  previous-frame transform anywhere. A moving object's history is caught only by the depth-plane
  tolerance — which rejects a large depth discontinuity but not an object sliding at roughly constant
  depth, e.g. a character walking across flat ground.
- **The same three inputs are what every other modern temporal technique wants.** This is not an
  NRD-specific tax.

## 3. The licence, which is a separate and independent blocker

NRD ships under the **NVIDIA RTX SDKs License**. GitHub reports its SPDX identifier as
`NOASSERTION` — it is not a recognised open-source licence.

`docs/ASSET_IMPORT.md` states this repository's policy: permissively-licensed only — MIT, BSD,
Apache-2.0, zlib, CC0, CC-BY. NRD fails that on its face.

There is also a hazard specific to *us* that would not apply to a game studio. The licence says a
licensee "may not distribute or sublicense the SDK as a stand-alone product". A game that links NRD
ships a game. **Aver is an engine**: it is redistributed to licensees who then build their own
products with it, which is much closer to sublicensing an SDK than to shipping an application. The
same clause that is routine for a game is a live question for an engine vendor.

For the avoidance of doubt about consistency: this is the identical reasoning already recorded for
**DLSS** in `third_party/fidelityfx-fsr/README.md`, which lists NVIDIA's proprietary SDK licence as
one of two independent blockers. Refusing NRD on the same grounds is not a new policy.

## 4. The permissively-licensed alternative has the same problem

**AMD FidelityFX Denoiser is MIT**, and this repository already vendors FidelityFX FSR 1 under
exactly that licence, so the precedent and the review are both already done. It ships a
spatio-temporal **shadow denoiser** built for at most one jittered shadow ray per pixel — which
describes `rtShadow` precisely — and a **reflection denoiser**.

It requires depth, **motion vectors** and normals.

So the licence is not what is actually gating this. **The G-buffer is.** Any denoiser worth vendoring
wants the same three buffers, and we have none of them.

## 5. What to do instead

**Do not vendor a denoiser. Build the prerequisite, then choose.**

The prerequisite is a thin G-buffer plus motion vectors, written by the existing forward pass as
extra render targets:

| Target | Format | Cost at 2750x1639 |
|---|---|---|
| Motion vectors | `RG16F` | ~18 MB |
| View-space depth | `R32F` | ~18 MB |
| Normal + roughness | `RGB10A2` or `RGBA8` | ~18 MB |

About **54 MB and 12 bytes per pixel** of bandwidth in the forward pass — set against the ~144 MB the
RT histories already cost at that resolution. Dynamic-object motion additionally needs a
previous-frame transform per instance, which is a small per-instance array, not a per-pixel cost.

What that one piece of work unlocks, all of it vendor-neutral:

- **FidelityFX Denoiser** (MIT) for shadows and reflections — or a hand-written À-Trous/SVGF filter,
  since SVGF is a published algorithm and RELAX is described by NVIDIA as an advanced version of it.
- **FSR 2/3** — blocked today on exactly this, per the FSR 1 README.
- **TAA**, which needs jitter plus the same motion vectors.
- **Screen-space reflections** as a fallback where there is no RT hardware.
- **Correct temporal reprojection for moving objects**, which fixes a defect we have *now*.

`docs/rendering/RENDERING.md` already names FidelityFX as the intended lever (SSSR for reflections,
Brixelizer GI, FSR for upscaling) and already lists motion vectors as an explicit integration task.
This investigation does not change that plan; it confirms it, and it identifies the single piece of
work every item on it is waiting behind.

## 6. What was done in the meantime (2026-08-27)

Not blocked on any of the above, because none of it needs a G-buffer:

- **`rtReflection` grew a real roughness lobe.** It traced a mirror ray and its caller concealed the
  mismatch twice over — refusing the reflection above roughness 0.5, and fading what survived toward
  flat sky at twice the roughness. It now opens a cone of `tan = rough^2` (the GGX alpha) using the
  same nested disc sequence, per-pixel rotation and frame jitter the sun-disc shadow already used. At
  roughness 0 the arithmetic reduces to the old mirror ray exactly, so glass and chrome are unchanged.
- **`rtReflectionSpatial` was added.** Reflections had no spatial filter at all. Its radius comes from
  roughness rather than from a host dial — the filter's width tracks the lobe's width — so it needs no
  new setting and no tier-ladder entry, and it is a strict no-op on a mirror.
- **Temporal accumulation was added to the untiled reflection path**, which is the one that actually
  runs, and only where the lobe introduced variance.
- **Both spatial kernels are now Gaussian rather than flat.** A flat kernel is a box filter, and a box
  filter rings — visible as a square-edged halo around a bright feature.
- **The reflection cutoff moved from 0.5 to 0.75 roughness**, with the fade demoted from mechanism to
  seam-hider across the last quarter of the range.

**Still true, and still worth fixing:** `rtShadowDenoise` remains 0 at every tier. The spatial shadow
filter is better than it was and still never runs. Turning it on is a measurement, not a code change,
and it should be made against a **moving** camera — every frame budget this project recorded before
2026-08-27 was taken with a parked one, which hid both the GI interval's ~10.5 ms benefit and the
shadow tile's artefact.

---

## Sources

- [NVIDIA-RTX/NRD](https://github.com/NVIDIA-RTX/NRD) — the library, its inputs and its denoiser set
- [NRD LICENSE.txt](https://raw.githubusercontent.com/NVIDIA-RTX/NRD/master/LICENSE.txt) — the NVIDIA RTX SDKs License
- [AMD FidelityFX Denoiser](https://gpuopen.com/fidelityfx-denoiser/) and its
  [1.3 manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/) — MIT, and its input requirements
- [GPUOpen-Effects/FidelityFX-Denoiser](https://github.com/GPUOpen-Effects/FidelityFX-Denoiser) — the shader source
- In-tree: `docs/ASSET_IMPORT.md` (licence policy), `third_party/fidelityfx-fsr/README.md` (the DLSS
  and FSR 2/3 precedent), `docs/rendering/RENDERING.md` (the FidelityFX plan)
