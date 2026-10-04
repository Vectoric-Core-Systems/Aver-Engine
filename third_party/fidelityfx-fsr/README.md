# AMD FidelityFX Super Resolution 1 (FSR 1)

| | |
|---|---|
| Upstream | https://github.com/GPUOpen-Effects/FidelityFX-FSR |
| Version | v1.0.2 |
| Licence | MIT (see `LICENSE.txt`, Copyright (c) 2021 Advanced Micro Devices, Inc.) |
| Vendored | 2026-08-09 |
| Contents | `ffx_a.h`, `ffx_fsr1.h` — header-only shader source, ~3,850 lines |

Two headers, no library, no binaries, no build system. They are included **from HLSL**, not from
C++: `ffx_a.h` is AMD's portable shader-intrinsics layer and `ffx_fsr1.h` is the algorithm.

## What this actually is

FSR 1 is **spatial**: it upscales one frame using only that frame. Two passes —

- **EASU** (Edge Adaptive Spatial Upsampling): a directionally and anisotropically adaptive
  Lanczos-like resample. This is the upscale.
- **RCAS** (Robust Contrast Adaptive Sharpening): applied *after* EASU, as a separate pass.

It needs the colour buffer and nothing else. No history, no motion vectors, no jitter.

## Why FSR 1 and not FSR 2 / 3

FSR 2 and 3 are **temporal** — they reconstruct detail by accumulating jittered frames, which is
what makes them dramatically better than any spatial filter and also what makes them unusable here
today. They require, per frame:

- a **sub-pixel jittered projection matrix** (Halton or similar), and
- **screen-space motion vectors for every pixel**, including dynamic objects, and
- depth, exposure, and a camera-cut reset signal.

This engine has none of those as whole-frame data. The only temporal machinery that exists is
scoped to ray-traced shadow history (`modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp`:
`prevViewProj` and its reprojection rect), which reprojects a shadow term, not the frame.

So the honest ordering is: FSR 1 works the day render scale lands, because render scale is its only
prerequisite. FSR 2/3 — and DLSS, and any hand-written TSR — all wait on the same vendor-neutral
work: jitter plus motion vectors. Doing that work once unlocks all of them.

## What is NOT here, and why

**NVIDIA DLSS.** Two independent blockers, either one sufficient:

1. **Licence.** The DLSS SDK ships under NVIDIA's proprietary SDK licence, and this repository
   accepts MIT / BSD / Apache-2.0 / zlib / CC0 / CC-BY only (`docs/ASSET_IMPORT.md`). It also
   requires redistributing `nvngx_dlss.dll` under those terms.
2. **Hardware.** DLSS runs on NVIDIA RTX (Turing and later) and uses tensor cores. The development
   machine here is an AMD Radeon RX 7800 XT, so it could not be executed, let alone verified.

The upscaler seam is deliberately shaped so DLSS *could* be implemented behind it by someone with
the hardware and an accepted licence — it takes depth, motion vectors and a jitter offset even
though FSR 1 ignores all three. The slot is empty on purpose, not by oversight.

## Updating

Replace the two headers from a tagged upstream release and update the version above. Nothing here is
modified from upstream; a diff against the tag should be empty.
