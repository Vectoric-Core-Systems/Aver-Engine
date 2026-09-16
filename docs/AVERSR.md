# Aver Super Resolution (AverSR)

The engine's upscaling subsystem: one user-facing name, several implementations behind a seam.

## What it is, and what it is not

**AverSR is a seam plus our own implementation.** It is not a wrapper that renames someone else's
work, and nothing in it may claim to be, or imply it is, FSR or DLSS.

| Backend | Status | Notes |
|---|---|---|
| Aver spatial upscaler (`SpatialUpscaler`) | **implemented and wired** | dependency-free Catmull-Rom bicubic resample; `needs()` answers `None` — no jitter, motion vectors or history |
| Aver edge-detecting AA (`AverSrFxaa`) | **implemented**, a second `IUpscaler` beside it | single-pass, luma-based, FXAA-style edge AA — anti-aliases an already-resolved (never-multisampled) target rather than changing resolution; written from the published technique, not a port |
| Aver temporal upscaler | **does not exist yet** | needs render-scale, jitter, per-pixel motion vectors, depth, exposure and a camera-cut reset as whole-frame data, none of which exists outside Voxi's ray-shadow-only reprojection — see Prerequisites, below |
| AMD FSR 1 | vendored (MIT), attributed, **not yet wrapped in an `IUpscaler`** | `third_party/fidelityfx-fsr`; the next implementation this module is meant to gain, alongside the two above, with no change to the seam itself |
| AMD FSR 2 / 3 | possible later | MIT; needs jitter + motion vectors |
| NVIDIA DLSS | **empty slot, on purpose** | see below |
| Intel XeSS | possible later | same prerequisites as FSR 2 |

> **This table used to list a temporal upscaler as "the default, and genuinely ours" and the spatial
> one as its fallback.** Neither was true of the tree: no `AverSrTemporal.*` exists anywhere under
> `modules/render.sr/`. The shipped default used to be **`Off`** — no upscaler ran at all unless
> `--aversr <level>` or the Editor Preferences combo asked for one. **Optimisation wave 2 (U2, 0.6)
> changed that: AverSR is now on by default at every Overall quality rung** — `Quality` at Epic and
> High, `Balanced` at Medium, `Performance` at Low. `Off` still exists and is still a selectable
> level; it is simply no longer what a project gets without asking. See "Default: Auto", below, for
> the per-rung table and the precedence a resolved level actually follows.
> What *is* built is the spatial resample and, since, a second `IUpscaler` implementation doing
> edge-detecting AA rather than resolution change — neither of those is a "fallback" for the other,
> they are the only two backends that exist. `IDevice::setUpscaler`/`upscaler()` and the D3D12/Vulkan
> composite-pass call site are real and wired (`modules/render.sr/README.md`, "Backend wiring"); a
> non-`Off` level was measured (PTTest, 2750×1639) taking the ray-driven primary pass from 5.0ms at
> `Off` to 2.3 / 1.7 / 1.3ms at Quality / Balanced / Performance. **SUSPECT**: that table's baseline
> was contaminated by a second renderer running concurrently (`aver-two-renderers-at-once.md`) and
> predates ReSTIR and NRD, both of which add their own pixel-bound cost that AverSR now also shrinks;
> `aver-aversr-measured.md` records the figure itself as unverified, and no composed-default number
> has replaced it (UNMEASURED). `SpatialUpscaler::execute()` has still never been proven against a
> live swapchain by a GPU capture, per the same source.

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

## Module boundaries

AverSR is **its own module** and is coupled to nothing, held to the standard
`modules/render.voxi` already meets. Voxi is the reference because it is the module in this engine
that most obviously could have been coupled and is not: it renders PBR materials, it drives the
sun, it owns GI -- and its CMakeLists still links only `Aver.Core`, `Aver.RHI` and
`Aver.Render.PBR.Materials`, with a comment spelling out that the dependency runs one way and
never the other.

### Targets

| Target | Kind | Links | Why |
|---|---|---|---|
| `Aver.Render.Sr` | STATIC | `Aver.Core`, `Aver.RHI` | the upscaler implementations |

`Aver.RHI` is the **generic** interface. Never `Aver.RHI.D3D12`, never `Aver.RHI.Vulkan` -- no
backend type may cross this boundary, and a link line is the only place that rule can actually be
enforced rather than merely intended.

Unlike Voxi there is no second SHARED target, because nothing P/Invokes the upscaler from C#. If
that ever changes, split it the way Voxi is split -- settings in a DLL depending on `Aver.Core`
alone, GPU work in a static library -- rather than putting `rhi::` types on a P/Invoke boundary.

### What AverSR must NEVER depend on

`Aver.Render.Voxi` · `Aver.Render.PBR` · `Aver.Scene` · `Aver.World` · `Aver.Landscape` ·
`Aver.Physics` · `Aver.Trifactor` · any RHI backend.

An upscaler takes a colour target, a depth target, motion vectors and a jitter offset, and produces
a bigger colour target. It has no legitimate reason to know what a material, an entity, a chunk or
a heightfield is, and the day it links one of them the seam has stopped being a seam.

### And no ENGINE MODULE may depend on AverSR

This is the half that is easier to get wrong, and the wording matters. Something has to construct
a concrete `SpatialUpscaler`, so "nothing links it" is impossible as stated. The precise rule is
the one Voxi already follows: **the HOST links it, the ENGINE does not**.

The `IUpscaler` **interface** lives in `Aver.RHI` beside `IRenderFeature`, not in this module.
`Aver.Render.Sr` provides implementations. A renderer holds an `IUpscaler*` that may be null and
calls it if it is not -- without linking, including, or knowing that AverSR exists. Only the
composition root (`sandbox`, and one day the game runtime) names the concrete type, behind
`if(TARGET Aver.Render.Sr)`, exactly as it already does for `Aver.Render.Voxi.Renderer` and
`Aver.Landscape.Renderer`.

So the test is not "does anything link it" but **"does any module under `modules/` link it"**.
Today the answer is no: `grep -rn 'Aver.Render.Sr' --include=CMakeLists.txt` returns the module's
own file and `sandbox/CMakeLists.txt`, nothing else.

The consequence, and the test: **`-DAVER_MODULE_SR=OFF` must build, link and render**, at native
resolution with no upscaler, and the pixels must be identical to a tree where the module was never
written. `AVER_MODULE_SR=1` is a PUBLIC compile definition on the target so consumers can `#if`
the whole feature out, and it reaches a translation unit ONLY through the link interface -- the
same mechanism, and the same failure mode, documented in `modules/world/CMakeLists.txt`.

### Enforcement, not intention

1. `option(AVER_MODULE_SR "..." ON)` in the root `CMakeLists.txt`.
2. An `sr-off` row in `scripts/module-matrix.ps1`, **in the same commit as the option**. That file
   says it plainly: *a module absent from this list is a module nobody checks*, and TRIFACTOR spent
   its whole life in exactly that gap.
3. `aver_check_module_dag()` catches an optional module named without an `if(TARGET ...)` guard.
4. A grep that no `sr::` type appears in a public header of any module that does not link it.

Modularity claimed in a comment is worth nothing; the matrix row is what makes it a fact.

## Quality levels

`Off` renders at native resolution and runs no upscaler at all — the pre-existing behaviour, and it
must stay bit-identical to having no AverSR in the build. It remains one of the five values a user,
a project or the CLI can pick; see "Default: Auto" for what a project gets when nobody picks one.

| Level | Render scale | Pixels vs native |
|---|---|---|
| `Off` | 1.00 | 100% |
| `Quality` | 0.67 | 45% |
| `Balanced` | 0.58 | 34% |
| `Performance` | 0.50 | 25% |

These map onto `IDevice::setRenderScale`; AverSR does not have a second resolution concept of its
own.

## Default: Auto

Optimisation wave 2 (U2, 0.6) put AverSR on by default at every Overall quality rung. `Auto` is not
a fifth render-scale level — it is a ladder lookup that resolves to one of the four levels above:

| Rung | Auto resolves to |
|---|---|
| Epic | `Quality` |
| High | `Quality` |
| Medium | `Balanced` |
| Low | `Performance` |

Epic and High take the gentlest level deliberately: those rungs are about image quality, and they
are where the GI ReSTIR visibility setting (`voxi.giRestirVisibility`) also defaults to `Full` — its
own most expensive mode. A project whose Overall combo reads Custom has no single rung to look up;
Auto then uses the ladder value at the higher of its GI and RT tiers.

**Precedence, highest first:**

1. CLI `--render-scale <factor>` — a literal scale, outranks everything.
2. CLI `--aversr off|quality|balanced|performance|auto`.
3. the editor's Display preference (`display.aversrChoice`: Auto / Off / Quality / Balanced /
   Performance / Manual scale). Picking `Auto` here defers to the source below, explicitly.
4. `RENDER.AVERSR` in the project manifest.
5. Auto — the table above, computed from the Overall rung (or Custom's GI/RT tiers).

The packaged game (`AverEngineRuntime.exe`) walks the same chain minus step 3: there is no Display
preference outside the editor, so a manifest value or Auto is all it has.

AverSR deliberately stays outside Custom detection — it lives at the module boundary above
`voxi::Settings` (see "Module boundaries", above), so a project reading Custom in the Overall combo
says nothing about its resolved upscaling level on its own. Where the two disagree, the editor's
Project Settings "Upscaling" line names the source and, when it differs from the rung's own default,
says so.

**Startup log.** Every run — interactive and `--frames` alike — logs the resolved level once, on
change or at startup:

    [AverSR] {level} ({source}): scene {sw}x{sh} -> present {w}x{h}; pass --aversr off for native captures

`{source}` is one of `Auto`, `Manifest`, `User` (the Display choice), `Cli`, or `ForcedOff` (a
device-loss cookie tripped on the previous launch forced this session's level to `Off`). Because
Auto applies to `--frames` runs the same as an interactive session, a capture or probe script that
needs native-resolution pixels has to say `--aversr off` explicitly rather than rely on the rung's
default — `scripts/gates.ps1`, `scripts/pt-compare.ps1` and `scripts/rt-spread.ps1` all do.

## Prerequisites

- **Spatial path**: render scale. That is all.
- **Temporal path**: render scale, sub-pixel jitter, per-pixel screen-space motion vectors, depth,
  exposure, and a camera-cut reset signal.

The temporal prerequisites are shared by FSR 2/3, XeSS and DLSS. Doing that work once unlocks every
one of them; it is the real blocker, not any vendor's SDK. See `docs/STATUS.md` for where it stands.
