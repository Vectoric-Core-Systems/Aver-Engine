# Aver Engine — Hardware Requirements & Minimum Specs

**Internal reference. This folder is not shipped to end users** — it exists so the launcher, store
page and support docs can be written from one authoritative source.

Everything here is derived from what the engine *actually checks at runtime*
(`D3D12Device::queryCaps`, `modules/rhi.d3d12/src/D3D12Device.cpp`), so this document and the
engine cannot drift apart silently. The launcher can query the same values — see
[Detecting this from the launcher](#detecting-this-from-the-launcher).

---

## 1. Tiers at a glance

| | **Minimum** | **Recommended** | **Ultra (ray tracing)** |
|---|---|---|---|
| **API** | DirectX 12, feature level 11_0 | DirectX 12, FL 11_0 | DirectX 12 Ultimate (FL 12_2) |
| **GPU (NVIDIA)** | GTX 600 series (Kepler) | GTX 1060 6GB (Pascal) | RTX 2060 (Turing) or newer |
| **GPU (AMD)** | HD 7000 / R7 260 (GCN 1.0) | RX 580 (GCN 4) | RX 6600 (RDNA 2) or newer |
| **GPU (Intel)** | HD 4400 (Haswell) | Arc A380 | Arc A750 or newer |
| **OS** | Windows 10 1803 | Windows 10 20H2 | Windows 10 2004 (May 2020) |
| **What you get** | Editor + PBR + shadow maps | + voxel GI at 128³ | + ray-traced shadows |

The engine **runs** on the Minimum tier. Features the hardware cannot do are reported
`Unsupported` and their settings refuse to enable — never silently no-op. See §5.

---

## 2. Feature → hardware requirement matrix

| Feature | Requires | Detected via | If missing |
|---|---|---|---|
| Editor, PBR, shadows | D3D12 FL 11_0 | `D3D12CreateDevice` | Engine will not start |
| MSAA 2×/4×/8× | Per-count quality levels | `MULTISAMPLE_QUALITY_LEVELS` | Falls back to highest supported, or Off |
| Voxel GI (Voxi) | Compute + `RWTexture3D` stores | core FL 11_0 | — (runs everywhere) |
| └ watertight voxelisation | Conservative raster tier ≥ 1 | `OPTIONS.ConservativeRasterizationTier` | Standard raster; thin geometry may miss voxels |
| **Ray tracing** | **DXR 1.1 + Shader Model 6.5** | `OPTIONS5.RaytracingTier` | Reported `Unsupported`; setting refuses |
| **Mesh shaders** | Mesh Shader Tier 1 + SM 6.5 | `OPTIONS7.MeshShaderTier` | Reported `Unsupported`; classic VS/GS path used |
| SM 6.x shaders (DXIL) | Shader Model 6.0 + `dxcompiler.dll` | `FEATURE_SHADER_MODEL` | Falls back to FXC/SM 5.1 |
| Path tracing | DXR 1.1 (same as above) | `OPTIONS5.RaytracingTier` | Reported `Unsupported` |

---

## 3. Why ray tracing targets DXR 1.1 (and only 1.1)

This is the decision that sets the Ultra tier, so it is worth stating the reasoning.

DXR has two tiers, and they are **not** two meaningfully different hardware populations:

- **DXR 1.0** — full ray tracing pipeline: `lib_6_3` shader libraries, state objects, shader
  binding tables, `DispatchRays`.
- **DXR 1.1** — adds **inline ray tracing** (`RayQuery`), usable from any shader stage. Needs
  Shader Model 6.5.

**AMD has never shipped a Tier-1.0-only GPU.** AMD entered ray tracing at Tier 1.1 with RDNA 2.
Intel entered at Tier 1.1 with Arc. On NVIDIA, every Turing-and-later part reports Tier 1.1.

So supporting DXR 1.0 *in addition* to 1.1 would buy exactly one thing: NVIDIA Pascal/Volta
(GTX 1060 6GB, GTX 1080, Titan V) and the cut-down Turing parts (GTX 1660/1650), which expose
DXR **through driver emulation with no RT cores** and run roughly an order of magnitude slower.
Reporting support does not make them usable — enabling ray tracing there would produce a slideshow
and a support ticket.

**Decision: target inline ray tracing (`RayQuery`, DXR 1.1) as the single path.** Benefits:

1. **Covers all real RT hardware** on AMD, NVIDIA and Intel with one code path.
2. **Far less machinery** — no state objects, no shader binding tables, no `DispatchRays`. Rays are
   traced directly from the existing pixel/compute shaders, so it composes with the current
   raster pipeline instead of replacing it.
3. **Honest hardware story** — the GPUs it excludes are the ones that could not run it acceptably
   anyway.

### On "DirectX 12 Ultimate"

D3D12 Ultimate is feature level **12_2**, which bundles DXR 1.1 + Mesh Shaders + Variable Rate
Shading Tier 2 + Sampler Feedback. It is a useful *marketing* label for the Ultra tier because the
hardware set is effectively identical to "has DXR 1.1".

**But the engine should keep gating on the specific cap (`RaytracingTier >= 1.1`), not on FL 12_2.**
Requiring 12_2 would additionally demand mesh shaders and sampler feedback that the renderer does
not use, so a driver or part exposing DXR 1.1 without the full 12_2 set would be excluded for no
reason. Advertise "DirectX 12 Ultimate class"; *check* the capability.

---

## 4. GPU support tables

### NVIDIA

| Generation | Example parts | Min tier | Ray tracing |
|---|---|---|---|
| Kepler (GTX 600/700) | GTX 660, 780 | ✅ Minimum | ❌ |
| Maxwell (GTX 900) | GTX 970, 980 | ✅ Minimum | ❌ |
| Pascal (GTX 10) | GTX 1060, 1080 | ✅ Recommended | ⚠️ Emulated, unusable — treated as ❌ |
| Turing GTX (16) | GTX 1650, 1660 | ✅ Recommended | ⚠️ No RT cores — treated as ❌ |
| **Turing RTX (20)** | RTX 2060, 2080 | ✅ | ✅ **Ultra** |
| **Ampere / Ada / Blackwell** | RTX 3060, 4070, 50-series | ✅ | ✅ **Ultra** |

### AMD

| Generation | Example parts | Min tier | Ray tracing |
|---|---|---|---|
| GCN 1–4 | HD 7970, RX 480, RX 580 | ✅ Minimum / Recommended | ❌ |
| Vega | Vega 56/64, Radeon VII | ✅ Recommended | ❌ |
| RDNA 1 (RX 5000) | RX 5600 XT, 5700 XT | ✅ Recommended | ❌ |
| **RDNA 2 (RX 6000)** | RX 6600, 6800 XT | ✅ | ✅ **Ultra** |
| **RDNA 3 / 4** | RX 7800 XT, RX 9000 | ✅ | ✅ **Ultra** |
| RDNA 2/3 iGPU | Radeon 680M / 780M | ✅ | ✅ Ultra (low throughput) |

> ⚠️ **All GCN parts** (through RX 500 / Vega) take a significant hit in voxel GI: the
> voxelisation pass uses a geometry shader, which GCN emulates through an off-chip ring buffer.
> GI is playable but noticeably more expensive there. The GS-free variant exists (`MSVoxel`, verified
> pixel-identical) but needs mesh shaders, which no GCN part has — so on this hardware the geometry
> shader is the only path, and that cost is real rather than avoidable.

### Intel

| Generation | Example parts | Min tier | Ray tracing |
|---|---|---|---|
| Haswell / Broadwell | HD 4400, HD 5500 | ✅ Minimum (Tier 1 binding — see §6) | ❌ |
| Skylake–Xe / Iris Xe | UHD 630, Iris Xe | ✅ Minimum | ❌ |
| **Arc Alchemist +** | A380, A750, B-series | ✅ | ✅ **Ultra** |

---

## 5. Behaviour on unsupported hardware

The engine never pretends. Every optional feature reports one of:

- **`Ready`** — implemented *and* supported; the setting does something.
- **`NotImplemented`** — the engine cannot do it yet (regardless of hardware).
- **`Unsupported`** — the GPU/driver cannot do it.

The editor greys out anything that is not `Ready`, and the setter **refuses** the value rather than
accepting it and doing nothing. This is why a user on an RX 580 sees ray tracing greyed out with
"Device has no ray tracing support" instead of a toggle that appears to work.

Concrete degradation path:

| Situation | Result |
|---|---|
| No DXR | Ray tracing / path tracing unavailable; **voxel GI still works** and remains the GI solution |
| No mesh shaders | Geometry goes through the input assembler; voxelisation uses the geometry shader. Pixel-identical |
| No conservative raster | GI still works; thin geometry may drop out of the volume |
| No DXC (`dxcompiler.dll` missing) | Everything above still works, compiled by FXC at SM 5.1 — see §5b |
| No 8× MSAA | Clamps to 4×/2×/Off |
| No D3D12 at all | Engine will not start in the default build (D3D11 is still a stub; Vulkan works but is off by default — see §6) |

Every row of that table has now been **executed and measured on this machine**, not merely reasoned
about — see §10 for exactly what that does and does not prove. A refused request is also stated in
the log, once, naming the capability that is missing: asking for `--ms` on a device without mesh
shaders used to leave the input-assembler path running with nothing anywhere saying the flag had been
turned down.

---

## 5b. Shader compilation and redistributables

Shaders compile at runtime through **DXC** (DXIL, shader model 6.x). DXC is required for mesh
shaders and for DXR's `RayQuery` — FXC tops out at SM 5.1 and cannot express either.

**`dxcompiler.dll` and `dxil.dll` must ship next to the executable.** They come from the Windows
SDK, are *not* part of Windows, and `dxil.dll` is the signing library — without it drivers reject
the compiled DXIL. The build copies both into `bin/` automatically.

If they are missing at runtime the engine logs a warning and **falls back to FXC/SM 5.1**: the
renderer still works, it just loses the SM6-only features. That fallback is deliberate so a
packaging mistake degrades instead of bricking the product.

Baseline is **SM 6.0**, which is broadly supported across D3D12 hardware with current drivers.
Mesh shaders and RayQuery compile at **SM 6.5** and only when the device reports the capability —
so moving to DXC did not raise the engine's hardware floor.

**The FXC path is a full renderer, not a stripped one.** Everything except mesh shaders and inline
RayQuery — the shadow map, voxel cone traced GI, the whole material system and its five maps — asks
for **SM 5.1** and gets compiled by FXC when DXC is absent. Measured: with `--force-caps no-dxc` all
seventeen oracle gates are **bit-identical** to the SM 6.6 hardware path. Nothing in those passes uses
anything SM 6.x introduced, and requiring 6.0 for them was gating the feature on a *compiler* rather
than on a capability. Ship `dxcompiler.dll` anyway — it is faster and it is the only way to get mesh
shaders or ray tracing — but a packaging mistake now costs those two features and nothing else.

## 6. OS and runtime requirements

| Requirement | Why |
|---|---|
| **Windows 10 1803** (April 2018) minimum | `IDXGIFactory6` for GPU-preference adapter selection. Older builds fall back to `EnumAdapters1`, so 1507+ *may* work but is untested. |
| **Windows 10 1809** for DXR 1.0 | DXR shipped in the October 2018 Update |
| **Windows 10 2004** (May 2020) for DXR 1.1 | Ships with DirectX 12 Ultimate |
| Windows 11 | Fully supported |

Current drivers are strongly recommended: DXR tier reporting on NVIDIA in particular changed across
driver revisions.

> **Not required:** Vulkan and D3D11. **D3D11 is still a stub** (`modules/rhi.d3d11/src/D3D11Device.cpp`
> is 13 lines). **Vulkan is no longer one** — see `modules/rhi.vulkan/README.md`: it presents a frame
> (grid, cube, shadow, sky, world axes) and the editor UI now draws on it too, behind
> `AVER_RHI_VULKAN`, a build option that **defaults ON** (`CMakeLists.txt:58`; this said "OFF by
> default" until 2026-09-20). **That changes why D3D12 is a hard requirement, and the honest answer is
> now less certain.** The default build does contain Vulkan code, so the old reason — "off by
> default" — no longer applies; but the backend is still selected by `--backend vulkan` rather than
> automatically, it still has 10 validation errors, and `docs/VULKAN.md` records that its cascaded
> shadow map has never been written. **Open question this pass did not settle:** whether a machine
> with a Vulkan driver and no D3D12 can actually run the editor end to end. Until somebody tries it,
> treat "supports DirectX 12" as a hard requirement — on the evidence of the known defects, not on
> the build flag.

---

## 7. Detecting this from the launcher

Do not hard-code GPU model lists in the launcher — model strings are a maintenance treadmill and
lie about rebrands. Query the capability instead. The engine already exposes it through the Voxi
C ABI, which the launcher can P/Invoke without starting the editor:

```csharp
using Aver.Scripting;

int tier = Voxi.RayTracingTier;   // 0 = none, 10 = DXR 1.0, 11 = DXR 1.1
bool ultraCapable = tier >= 11;

int maxAA = Voxi.MaxMsaa;         // 1, 2, 4 or 8
string why = Voxi.StatusTextOf(VoxiFeature.RayTracing);  // user-facing reason
```

Native equivalent: `aver_voxi_ray_tracing_tier()` / `aver_voxi_max_msaa()` from
`modules/render.voxi/include/aver/voxi/voxi_abi.h`.

> **Caveat:** a standalone launcher process gets its own copy of `Aver.Render.Voxi.dll` with empty
> device caps until something pushes `DeviceInfo` into it. To probe hardware from the launcher the
> caps must be populated there — either by hosting a minimal D3D12 device or by having the engine
> write a capability report on first run. Wire this up before shipping the launcher check.

---

## 8. Suggested store / launcher copy

**Minimum**
> OS: Windows 10 64-bit (version 1803 or later)
> Graphics: DirectX 12 capable GPU — NVIDIA GTX 600 series, AMD Radeon HD 7000 series, or Intel HD 4400
> Notes: Global illumination and ray tracing unavailable or reduced

**Recommended**
> OS: Windows 10 64-bit (20H2 or later)
> Graphics: NVIDIA GTX 1060 6GB, AMD Radeon RX 580, or better
> Notes: Voxel global illumination at full quality

**Ultra — ray tracing**
> OS: Windows 10 64-bit (version 2004 or later) / Windows 11
> Graphics: DirectX 12 Ultimate GPU — NVIDIA RTX 2060, AMD Radeon RX 6600, or Intel Arc A750 or better
> Notes: Hardware ray tracing requires DXR 1.1. GTX 16-series and GTX 10-series are not supported for ray tracing.

---

## 9. Maintenance

When a renderer feature gains or loses a hardware requirement, update **all three**:

1. `D3D12Device::queryCaps` — the runtime check
2. This document — §2 matrix and §4 tables
3. `scripts/gates.baseline.txt` — the measured behaviour, via `./scripts/gates.ps1 -Record`

If they disagree, the runtime check is correct by definition; fix the doc.

---

## 10. What has actually been tested, and what has not

This section exists because "supports X" and "has been observed doing X" are different claims, and
only the second one is worth anything to a user. Be exact about which is which.

**Tested, on real hardware:** one GPU. An **AMD Radeon RX 7800 XT** (RDNA 3, driver
32.0.23027.2005), reporting MSAA to 8×, DXR tier 1.1, mesh-shader tier 1, SM 6.6, conservative
raster, typed UAV loads and resource-binding tier 3 — i.e. every capability the engine uses.

**Tested, by reducing what the device reports** (`--force-caps`, see `docs/STATUS.md` §4g): the
no-ray-tracing, no-mesh-shader, no-conservative-raster, SM 6.0 and FXC/SM 5.1 paths, plus a
resource-binding-tier-1 and no-typed-UAV report, each across the full gate set with the D3D12 debug
layer draining into the log. `./scripts/gates.ps1` re-runs all of it.

**Tested, on a second implementation of D3D12:** **WARP**, the software rasteriser. It is not
different *hardware* — it reports the same tiers as the RX 7800 XT — but it is an entirely
independent implementation, it agrees with the hardware to the raw 8-bit code at every gate, and it
found a fault the hardware path absorbs silently.

**NOT tested, and no clamp can substitute for it:**

- **NVIDIA and Intel silicon, and their drivers.** Nothing here has run on either. Vendor-specific
  behaviour — shader compilation quirks, DXR tier reporting across driver revisions, mesh-shader
  scheduling — is exactly what a clamp on our own AMD driver cannot reach.
- **Real resource-binding Tier 1 hardware.** The clamp makes the engine *report* Tier 1, but the
  D3D12 runtime validates against the **real** device tier, which is 3 on both adapters available
  here. The Tier 1 discipline the code follows (every declared table bound on every pass, every heap
  slot null-filled by declared kind, every declared root CBV given an address) is therefore
  reasoned and unmeasured. Haswell/Broadwell Intel is the hardware that would settle it.
- **Old GCN parts**, where the geometry shader used by voxelisation is emulated. The mesh-shader
  variant that avoids it exists and is verified pixel-identical, but no GCN part has run either.
- **Anything at scale.** One editor scene, one cube, one ground plane.

A clamped capability is a weaker claim than a different GPU, and this document will keep saying so
until a second vendor's part has actually run the gates.
