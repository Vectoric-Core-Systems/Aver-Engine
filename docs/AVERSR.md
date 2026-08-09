# Aver Super Resolution (AverSR)

The engine's upscaling subsystem: one user-facing name, several implementations behind a seam.

## What it is, and what it is not

**AverSR is a seam plus our own implementation.** It is not a wrapper that renames someone else's
work, and nothing in it may claim to be, or imply it is, FSR or DLSS.

| Backend | Status | Notes |
|---|---|---|
| Aver temporal upscaler | the default, and genuinely ours | needs jitter + motion vectors |
| Aver spatial upscaler | fallback when temporal data is unavailable | plain resample, no history |
| AMD FSR 1 | vendored, MIT, attributed | `third_party/fidelityfx-fsr`, spatial only |
| AMD FSR 2 / 3 | possible later | MIT; needs jitter + motion vectors |
| NVIDIA DLSS | **empty slot, on purpose** | see below |
| Intel XeSS | possible later | same prerequisites as FSR 2 |

**Upscalers are not combined.** One runs per frame. Running two costs double and they fight over the
same history — "merge FSR and DLSS into one better upscaler" is not a thing that exists.

### Why the DLSS slot is empty

Two independent reasons, either sufficient:

1. **There is no source to integrate.** DLSS ships as a signed binary (`nvngx_dlss.dll`) plus a thin
   API header. It cannot be read, merged or modified — only called.
2. **Licence and hardware.** NVIDIA's SDK licence is not on this repository's accepted list
   (`docs/ASSET_IMPORT.md`: MIT / BSD / Apache-2.0 / zlib / CC0 / CC-BY), and DLSS requires NVIDIA
   RTX tensor cores. The development machine is an AMD Radeon RX 7800 XT, so it could not be run,
   let alone verified.

The seam still accepts depth, motion vectors and a jitter offset that our spatial path ignores,
precisely so the slot stays implementable by someone with the hardware and an accepted licence.

### Attribution is not optional

FSR is MIT. MIT permits modification and redistribution and requires the copyright notice and
licence text travel with the derived work. Code derived from FSR keeps `third_party/fidelityfx-fsr/
LICENSE.txt` and says so at the top of the file. Rebranding derived code as wholly ours would break
the one condition that licence has, and this repository is permissive-only specifically so it never
has to have that argument.

## Naming

Branding is stamped at every layer, following the conventions the engine already uses — `Aver.*`
targets, `aver::*` namespaces, `AVER_*` macros, `aver_<subsystem>_*` C ABI, and a distinct
subsystem word the way `Trifactor` names the virtualized-geometry module.

| Layer | Pattern | Example |
|---|---|---|
| Brand, in UI / logs / docs | Aver Super Resolution, AverSR | `[AverSR] Balanced: 1766x994 -> 3532x1987` |
| CMake target | `Aver.Render.Sr` | |
| Namespace | `aver::sr` | `aver::sr::IUpscaler` |
| Public headers | `include/aver/sr/AverSr*.hpp` | `AverSrUpscaler.hpp` |
| Sources | `src/AverSr*.cpp` | `AverSrTemporal.cpp` |
| Types | unprefixed inside `aver::sr` | `sr::Quality`, `sr::UpscaleDesc` |
| C ABI | `aver_sr_*` | `aver_sr_set_quality()` |
| Module switch | `AVER_MODULE_SR` | needs a `scripts/module-matrix.ps1` row |
| Shader entry points | `AverSr*` | `AverSrTemporalResolveMain` |
| Log tag | `[AverSR]` | |

**Not `aver::aversr` or `AVER_MODULE_AVERSR`.** The `aver` is already in the namespace root and the
macro prefix; doubling it reads as a stutter. The brand is carried by the file names, the type-level
`AverSr*` symbols, the `aver_sr_*` ABI and the `[AverSR]` log tag — which is every place it is
externally visible.

**What branding does and does not buy.** It does not prevent anyone copying the code; symbols can be
renamed by whoever takes them. What it does is make provenance obvious — lifted code carries
`AverSrTemporalResolveMain` and `aver_sr_set_quality` into someone else's binary, where it is
trivially identifiable. That is worth having, and it is the honest reason to do it.

## Quality levels

`Off` renders at native resolution and runs no upscaler at all — the pre-existing behaviour, and it
must stay bit-identical to having no AverSR in the build.

| Level | Render scale | Pixels vs native |
|---|---|---|
| `Off` | 1.00 | 100% |
| `Quality` | 0.67 | 45% |
| `Balanced` | 0.58 | 34% |
| `Performance` | 0.50 | 25% |

These map onto `IDevice::setRenderScale`; AverSR does not have a second resolution concept of its
own.

## Prerequisites

- **Spatial path**: render scale. That is all.
- **Temporal path**: render scale, sub-pixel jitter, per-pixel screen-space motion vectors, depth,
  exposure, and a camera-cut reset signal.

The temporal prerequisites are shared by FSR 2/3, XeSS and DLSS. Doing that work once unlocks every
one of them; it is the real blocker, not any vendor's SDK. See `docs/STATUS.md` for where it stands.
