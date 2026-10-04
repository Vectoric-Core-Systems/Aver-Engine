# NVIDIA SDK usage and licence compliance

**No NVIDIA SDK is vendored, built, shipped or packaged any more.** This page is now a short record
of how that came about. It is not legal advice and was not written by a lawyer.

## 1. History and what replaced each SDK

| Formerly vendored | Removed | Replaced by |
|---|---|---|
| RTXDI and RTXGI (SHaRC) | Earlier change | In-house ReSTIR GI (`modules/render.voxi/shaders/voxi_reservoir.hlsli`, `voxi_restir.hlsli`) |
| NVIDIA NRD (with MathLib and ShaderMake, which existed only for it) | The change that introduced `Aver.Render.Denoise` | AMD FidelityFX Denoiser (MIT), `third_party/fidelityfx-denoiser`, driven by `modules/render.denoise` |

- Nothing NVIDIA ships in builds or packages: no NVIDIA library is linked, no NVIDIA shader is
  compiled, and no NVIDIA licence text needs to travel with a package. The denoiser's FidelityFX
  headers deploy to `bin/shaders/FidelityFX` together with their MIT `LICENSE.txt`.
- The NVIDIA notices script (`scripts/NvidiaNotices.ps1`) and the packaging redistribution gate that
  blocked `stage-payload.ps1` and `stage-game.ps1` are gone, because there is nothing left for them
  to check.
- NVIDIA remains a supported GPU vendor. That is a hardware statement and carries no licence
  obligation.
- If an NVIDIA SDK is ever vendored again, re-read its licence text rather than assuming it matches
  the earlier ones, and reintroduce a notices and redistribution check before it ships.

---

## 2. Patents and provenance of the in-house ReSTIR GI

This is a neutral record of facts, not legal advice.

- ReSTIR GI in `modules/render.voxi/shaders/voxi_reservoir.hlsli` and `voxi_restir.hlsli` is Aver's
  own code, written from the papers: Talbot 2005 (RIS), Bitterli 2020 (ReSTIR), Ouyang 2021 (ReSTIR
  GI), Lin 2022 (GRIS), Jarzynski and Olano 2020 (PCG hash) and Cigolle 2014 (octahedral normals).
  It is not derived from RTXDI source, and no RTXDI or RTXGI file remains in the tree.
- NVIDIA holds US 11,315,310 (ReSTIR plus a GI data structure) and US 12,299,801 (ReGIR). Aver's
  code adds no per-cell or world-space stochastic light reservoirs (no ReGIR-style grid); reservoirs
  are per pixel only.
- Removing the NVIDIA source does not itself resolve any patent question. Whether the in-house
  method falls within any claim has not been assessed here.
