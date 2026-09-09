# Denoising, and why NVIDIA NRD is not the answer here

> **SUPERSEDED ON THE LICENCE QUESTION, 2026-09-09.** NRD is now vendored at `third_party/nrd`. The
> owner was shown this document's licence argument — that the NVIDIA RTX SDKs License grants
> distribution only "as incorporated in object code format into a software application" and without
> the right to sublicense, so every Aver licensee would need their own grant — and accepted it
> anyway. `third_party/nrd/AVER_README.md` records the decision and the flow-through consequence for
> Aver's own EULA.
>
> **Everything else on this page still stands**, including the technical analysis of what NRD needs
> from a renderer and the description of what the hand-written filters do. Read the licence section
> below as history rather than as current policy.

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

One of those was switched off in everything we shipped at the time this page was investigated, which
is worth stating plainly because it was easy to look at the code and conclude otherwise:

- **`rtPixelsPerRayTileForQuality` returns 1 at every tier**, which forces `tileBits = 0`, so the
  "reuse last frame's ray" path is dead. Every pixel traces a fresh shadow ray every frame.

**`rtShadowDenoiseForQuality` no longer returns 0 at every tier — this page originally said it did,
and by the time it was committed that was already wrong.** The same commit that added this file
(`31c06a4`) also changed the radius rungs elsewhere in `Voxi.cpp`, from 0 at every tier to `Low`/
`Medium`/`High` = 2, `Epic` = 1 (`Off` stays 0, correctly, since RT is not running to have anything
to filter). Nothing in that commit's own message mentions the change, and this page never caught up
to its own sibling edit. The spatial shadow filter is therefore a **live pass in the shipped product**,
not a permanent no-op — see the re-measurement note at the end of §6.

So the honest description of the shipped denoiser is now: **a temporal reprojection for shadows, a
spatial shadow filter that runs at every tier except Off, and a full spatial+temporal pipeline for
reflections (§6).**

## 2. What NRD would need, and what this renderer has

NRD resolves noise using per-pixel G-buffer guides. Its three required inputs are:

| NRD input | What it is | Does Aver have it? |
|---|---|---|
| `IN_MV` | Motion vectors; NVIDIA recommends 2.5D or 3D over 2D | **Declared and implemented, but dormant** — see the correction below. |
| `IN_VIEWZ` | Linearised view-space depth of primary hits | **Declared and implemented, but dormant** — same correction. |
| `IN_NORMAL_ROUGHNESS` | World normal + linear roughness + material ID, packed | **Declared and implemented, but dormant** — same correction. |

That was not a near miss when this page was written, and the practical answer today is still "not
usable" — but the reason changed underneath this page without the page saying so. **The very commit
that added this file (`31c06a4`) also added the prerequisite §5 calls for**: `IDevice::
setGBufferEnabled`, `gBufferVelocityTexture()`, `gBufferViewZTexture()` and
`gBufferNormalRoughnessTexture()` (`RHI.hpp`), fully implemented — not just declared — in
`D3D12Device.cpp` (velocity RG16F, view-space depth R32F, normal+roughness RGB10A2, ~54 MB when on,
matching this page's own §5 estimate almost exactly). **Nothing calls `setGBufferEnabled(true)`
anywhere in the tree**, so it defaults off, allocates nothing, and every existing build renders
bit-identically to before it existed — and the Vulkan backend has no implementation at all, only the
inert base-class default. So "Aver is a forward renderer with no G-buffer" is no longer quite right;
"Aver has a G-buffer nothing turns on" is the current, more precise statement, and `PSMainVoxi` itself
is unchanged — it still returns a single `SV_TARGET`, and the G-buffer above is written by the
backend's own pass, not by that shader.

The declaration `UpscalerNeeds::MotionVectors` exists in `RHIResources.hpp`, and its comment no
longer says nothing produces them — it says `gBufferVelocityTexture()` writes exactly this quantity
whenever the (still-never-enabled) G-buffer is on, and that wiring it into `UpscalerInput::
motionVectors` is "the next step, not this one," citing this very page. That next step has not been
taken as of this correction.

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
exactly that licence, so the precedent and the review are both already done — and, as of the same
day this page was investigated, so is the vendoring itself: it now also lives at
`third_party/fidelityfx-denoiser` (header-only HLSL, no C++ side), its own README reaching the same
conclusion this page does. It ships a spatio-temporal **shadow denoiser** built for at most one
jittered shadow ray per pixel — which describes `rtShadow` precisely — and a **reflection denoiser**.

It requires depth, **motion vectors** and normals.

So the licence is not what is actually gating this. **The G-buffer is — or rather, was; see §2's
correction.** Any denoiser worth vendoring wants the same three buffers. We now have them declared
and, on D3D12, implemented; nothing calls `setGBufferEnabled(true)` to turn them on, and nothing
reads them once on, so the practical answer is still "not usable" even though "we have none of them"
is no longer the reason why.

## 5. What to do instead

**Do not vendor a denoiser. Build the prerequisite, then choose.**

**Half of this has since happened, in the very commit that added this page (`31c06a4`), without this
page being updated to say so — see the corrections in §2 and §4.** The denoiser (FidelityFX Denoiser,
MIT) is vendored. The prerequisite is declared and, on D3D12, implemented. What has NOT happened is
either half being turned on or wired to a consumer: no code calls `setGBufferEnabled(true)`, no pass
populates `UpscalerInput::motionVectors` from it, and Vulkan has no G-buffer implementation at all.
The description immediately below is therefore still the accurate statement of what remains to be
*built* in the sense of "connected and exercised," even though the raw render-target plumbing it
describes already exists in the D3D12 backend:

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

**No longer true, and this page said so for a while after it stopped being true:** `rtShadowDenoise`
does not remain 0 at every tier — see the correction in §1. The spatial shadow filter, better than it
was, now also runs (radius 2 at Low/Medium/High, 1 at Epic). Whether that turn-on was actually
measured against a **moving** camera, the way this paragraph originally called for, is not stated
anywhere in the commit that made the change (`31c06a4`) and this page finds no later measurement
either — every frame budget this project recorded before 2026-08-27 was taken with a parked camera,
which hid both the GI interval's ~10.5 ms benefit and the shadow tile's artefact, and nothing in the
tree today says that gap has since been closed for this specific knob.

---

## Sources

- [NVIDIA-RTX/NRD](https://github.com/NVIDIA-RTX/NRD) — the library, its inputs and its denoiser set
- [NRD LICENSE.txt](https://raw.githubusercontent.com/NVIDIA-RTX/NRD/master/LICENSE.txt) — the NVIDIA RTX SDKs License
- [AMD FidelityFX Denoiser](https://gpuopen.com/fidelityfx-denoiser/) and its
  [1.3 manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/) — MIT, and its input requirements
- [GPUOpen-Effects/FidelityFX-Denoiser](https://github.com/GPUOpen-Effects/FidelityFX-Denoiser) — the shader source
- In-tree: `docs/ASSET_IMPORT.md` (licence policy), `third_party/fidelityfx-fsr/README.md` (the DLSS
  and FSR 2/3 precedent), `docs/rendering/RENDERING.md` (the FidelityFX plan),
  `third_party/fidelityfx-denoiser/README.md` (the vendored denoiser itself, now in-tree),
  `modules/rhi/include/aver/rhi/RHI.hpp` (the G-buffer declaration, `IDevice::setGBufferEnabled`)
