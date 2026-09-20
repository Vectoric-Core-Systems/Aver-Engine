# NVIDIA SDK usage and licence compliance

**Last audited: 2026-09-21.** Against commit `a05275e2` plus the working tree at that date.

This document records which NVIDIA SDKs this engine contains, which of them actually ship, what
NVIDIA's licence requires of us for each, and — separately and plainly — **whether we currently do
it**. It exists so the answer to "are we compliant?" is a file you can re-read rather than an
investigation you have to repeat.

**It is not legal advice and was not written by a lawyer.** Every obligation below is quoted
verbatim with its source line so you can read the clause yourself. Where a clause is ambiguous this
document says so rather than picking a reading.

---

## 1. What is in the tree, and what actually ships

Vendored and shipped are different questions, and only the second one creates obligations for a
distributed product. Both are recorded because the source repository is itself distributed under
`LICENSE.md`.

| SDK | Path | Built? | Ships in editor? | Ships in packaged game? | How |
|---|---|---|---|---|---|
| **NRD** (Real-Time Denoisers) | `third_party/nrd` | Yes — `AVER_WITH_NRD` defaults ON | Yes | Yes | Compiled, statically linked into `Aver.Render.NRD` → `Aver.Render.Voxi.Renderer` → both `Sandbox.exe` and `AverEngineRuntime.exe` |
| **RTXDI** (ReSTIR DI/GI) | `third_party/rtxdi` | Its C++ is **never** built | Yes | Yes | **As verbatim HLSL source.** `modules/render.voxi/CMakeLists.txt:84` `file(COPY)`s `Include/Rtxdi` whole into `bin/shaders/Rtxdi`, which the package layout ships as `shaders\**` |
| **RTXGI** (SHaRC) | `third_party/rtxgi` | No | No | No | Nothing in the tree references it. See §5 |
| **MathLib** | `third_party/mathlib` | Header-only, via NRD | Yes | Yes | NRD's dependency |
| **ShaderMake** | `third_party/shadermake` | Build-time tool | No | No | Compiles NRD's shaders during the build |

**RTXDI is the strongest case for attribution, not NRD.** NRD ships as compiled object code;
RTXDI ships as *human-readable NVIDIA source text sitting on disk* in every build and every package.
Anyone who opens `bin/shaders/Rtxdi/GI/Reservoir.hlsli` is reading NVIDIA's source.

**What each is actually used for.** NRD denoises the ray-traced global-illumination signal
(`Voxi NRD denoise` in the per-pass GPU breakdown). RTXDI *is* the ReSTIR GI estimator selected by
`--gi-mode 1` — `modules/render.voxi/shaders/voxi_restir.hlsli` includes `Rtxdi/GI/Reservoir.hlsli`
and `Rtxdi/GI/SpatioTemporalResampling.hlsli` and calls into them. This is not an optional garnish;
it is one of the engine's two indirect-diffuse paths.

---

## 2. The obligations, and where we stand

Licence texts live at `third_party/nrd/LICENSE.txt`, `third_party/rtxdi/LICENSE.txt` and
`third_party/rtxgi/License.md`. NRD's and RTXDI's are the same "NVIDIA RTX SDKs License" with an
"NVIDIA RTX SUPPLEMENT" appended; the supplement governs where the two conflict.

### 2.1 The source notice — **MET (source distribution only)**

> "The following notice shall be included in modifications and derivative works of source code
> distributed: *This software contains source code provided by NVIDIA Corporation.*"
> — `third_party/nrd/LICENSE.txt:41-43`

The notice is reproduced verbatim in `LICENSE.md` under **NVIDIA SOFTWARE DEVELOPMENT KITS**.

**Still open:** a *packaged game* receives no notice at all. See §3.

### 2.2 Attribution — **MET, by not having a credit screen**

**Read clause 6.1(c), not 6.1(b).** There are two trademark-placement clauses and they cover
different SDKs. Getting this wrong once already cost an afternoon's worth of the wrong plan.

> **(b)** "NVIDIA Trademark Placement in Applications with the DLSS SDK or NGX SDK. For
> applications that incorporate the DLSS SDK or NGX SDK or portions thereof, you must attribute the
> use of the applicable SDK and include the NVIDIA Marks on splash screens, in the about box of the
> application (if present), and in credits for game applications."
> — `third_party/nrd/LICENSE.txt:313-318`

That is the clause naming splash screens and about boxes, and **it does not apply to this product**,
because neither the DLSS SDK nor the NGX SDK is included (§2.6). The clause that governs NRD and
RTXDI is the next one:

> **(c)** "NVIDIA Trademark Placement in Applications with a licensed SDK, other than the DLSS SDK
> or NGX SDK. For applications that incorporates and/or makes use of a licensed SDK, other than the
> DLSS SDK or NGX SDK, you must attribute the use of the applicable SDK and include the NVIDIA Marks
> on the credit screen for applications that have such credit screen, or where a credit screen is
> not present prominently in end user documentation for the application."
> — `third_party/nrd/LICENSE.txt:320-325`

**The decision taken (2026-09-21): this product has no credit screen, so attribution lives in
end-user documentation — `LICENSE.md` and this file.** A credit screen was considered and
deliberately dropped, because having one engages the NVIDIA Marks requirement, and Mark usage is
separately gated on style/colour/typeface specifications plus prior written approval of a sample
use. Not having a credit screen routes the obligation to documentation, which costs nothing and
needs no approval.

**No NVIDIA Marks are used.** The NVIDIA name appears as plain text identifying the SDKs.

**The ambiguity, recorded rather than resolved.** Clause (c)'s second branch is punctuated as "or
where a credit screen is not present prominently in end user documentation", which can be read
either as *"or, where a credit screen is not present, attribute prominently in end-user
documentation"* (the plain reading, and the one relied on here) or as carrying the Marks
requirement into the documentation branch too. This document does not claim to settle that. If the
stricter reading is ever required, the route is NVIDIA's Mark-approval process, not a credit screen.

**Consequence for the editor's About box:** none. It is not a credit screen, and clause (b) — the
one that names about boxes — governs only DLSS/NGX applications. No change is owed there.

### 2.3 Material additional functionality — **MET**

> "An application must have material additional functionality, beyond the included portions of the
> SDK." — `third_party/nrd/LICENSE.txt:38-39`

Aver Engine is a game engine; the SDKs are two components of its renderer.

### 2.4 Onward distribution terms — **MET for the source licence**

> "You agree to distribute the SDK subject to the terms at least as protective as the terms of this
> license…" — `third_party/nrd/LICENSE.txt:45-49`

`LICENSE.md` is an evaluation licence that forbids redistribution outright, which is at least as
protective. **This changes the day the engine ships under different terms** — revisit then.

### 2.5 Interoperability — **UNVERIFIED (reads as satisfied)**

> "Your applications that incorporate, or are based on, the SDK must be fully interoperable with
> compatible GPU hardware products designed by NVIDIA or its affiliates."
> — `third_party/nrd/LICENSE.txt:259-261`

Nothing gates these features to non-NVIDIA hardware: NRD builds with whichever offline DXC is
present (Windows SDK or Vulkan SDK, vendor-neutral), and `modules/render.voxi` links only the
vendor-agnostic `Aver.RHI`, never a backend.

**This is a reading of the source, not a test.** Development happens on an AMD Radeon RX 7800 XT
and the engine has never been run on NVIDIA hardware by anyone recorded here. Treat this row as
unverified until somebody runs it on an NVIDIA GPU.

### 2.6 DLSS / NGX — **NOT APPLICABLE**

Neither SDK is vendored. Every occurrence of "DLSS" or "NGX" in the tree is NVIDIA's own licence
text, upstream documentation, or this project's notes on why DLSS was deliberately not integrated.

Consequently **none** of the DLSS/NGX-specific terms apply: no pre-commercial-release notification
obligation, no NVIDIA-GPU-only development restriction, no cloud-service limitation, no
over-the-air-update terms. **If DLSS or NGX is ever integrated, every one of those returns** — the
notification one in particular requires contacting NVIDIA *before* commercial release.

### 2.7 MathLib and ShaderMake (MIT) — **MET in source, OPEN in packages**

Both are NVIDIA-authored and MIT-licensed. MIT requires the copyright notice and the permission
notice to accompany redistribution. Their texts are intact in their directories and are listed in
`LICENSE.md`. The packaging gap in §3 applies to them too.

---

## 3. The gap that is still open

**No packaged game ships any third-party notice — NVIDIA or otherwise.**

`scripts/stage-payload.ps1` and `scripts/stage-game.ps1` both build a `THIRD-PARTY-NOTICES.txt`,
and neither mentions NVIDIA, NRD, RTXDI, MathLib or ShaderMake. Verified by grep: both score zero.

This matters most for RTXDI, whose source text is physically present in the package, and for the
MIT components, whose licences require the notice to travel with the distribution.

`LICENSE.md` covers the **source repository**. It does not travel with a built game.

**What would close it:** add the NVIDIA components and the MIT components to the `$components`
arrays both staging scripts already use to generate `THIRD-PARTY-NOTICES.txt`.

### 3.1 Until then, packaging is BLOCKED

**Decision, 2026-09-21: rather than produce packages that are not clear to distribute, the
packaging path refuses.** The engine is in beta and nothing is shipping, so stopping costs nothing
and is the honest answer; producing a package that quietly omits required notices would not be.

Both `scripts/stage-game.ps1` and `scripts/stage-payload.ps1` exit 2 with the reason before doing
any work. That covers the editor's **File ▸ Package Project…** too, since it shells out to
`stage-game.ps1` rather than staging anything itself.

`stage-payload.ps1` is gated as well as `stage-game.ps1`, and deliberately: it stages the ENGINE for
a developer rather than a GAME for a player, but it redistributes the same compiled NRD and the same
verbatim RTXDI source, so the same obligation attaches to it.

**`-IAcceptNvidiaRedistribution` lifts the block** without editing either script, and prints a loud
line into the log when used so a package built that way is identifiable afterwards. It exists
because a hard-coded refusal is the kind of thing somebody deletes in a hurry, and because the day
clearance arrives the response should not be a source change. **It is not a way to skip the work.**

**To lift the block properly:** close §3, then delete the gate from both scripts.

---

## 4. Re-running these checks

None of this needs a build or a running engine. From the repo root:

```bash
python scripts/module-guard-audit.py            # unrelated, but the same "check, don't assume" habit
grep -ci nvidia scripts/stage-payload.ps1 scripts/stage-game.ps1   # expect > 0 once §3 is closed
# NOT a check: the About box is deliberately NOT an attribution surface -- see §2.2.
grep -c NVIDIA LICENSE.md                                           # expect > 0 (currently satisfied)
grep -rl "rtxgi\|sharc" --include=CMakeLists.txt --include=*.hlsl . # expect empty while §5 holds
```

And the block itself, which should refuse with exit 2 until §3 is closed:

```bash
pwsh ./scripts/stage-game.ps1 -Project <any.ocproject> -Out <tmp>   # expect: PACKAGING BLOCKED, exit 2
```

**When the vendored SDKs are updated, re-read the licence texts rather than assuming they are
unchanged.** The RTXDI and NRD copies already differ in formatting and in which SDKs their preamble
enumerates, which means NVIDIA does revise these documents between drops.

---

## 5. RTXGI: vendored, not wired

RTXGI is in the tree and nothing references it — no `CMakeLists`, no shader, no source file.

It is listed in `LICENSE.md` because that table describes what the **repository contains**, which is
accurate. It creates no distribution obligation while nothing ships it.

Two facts worth keeping here so they are not rediscovered:

- **RTXGI 2.x is not the DDGI probe SDK.** NVIDIA dropped probe-based irradiance caching; 2.x is
  SHaRC (a world-space hashed radiance cache) plus NRC (a neural radiance cache). Looking for DDGI
  in it is a dead end.
- **NRC cannot run on this development machine.** It trains a network per frame on Tensor Cores and
  requires NVIDIA Turing or later. Only SHaRC is a candidate here, and that is a hardware limit
  rather than a licensing one.

**Wiring SHaRC changes §1 and §3 but not §2** — the obligations above already apply through NRD and
RTXDI, so the only change is that a third SDK joins the lists.

### 5.1 What has been done toward wiring it (2026-09-21)

The *compatibility* half is in; the algorithm is not. The split is deliberate: everything below can
be verified today, and the part left out cannot.

**Done:**

- **SHaRC's four headers deploy**, structure-preserving, into `bin/shaders/Sharc/`
  (`modules/render.voxi/CMakeLists.txt`). All four are `.h`, which `aver_deploy_shaders` cannot see
  — it globs `*.hlsl`/`*.hlsli` and flattens — so the naive route deploys nothing and fails at
  RUNTIME, since HLSL compiles at runtime here. Under their own directory, because `<exe>/shaders`
  is a case-insensitive flat namespace that has already silently swallowed one file.
- **`DeviceCaps::shaderInt64Atomics`**, asked of the device rather than inferred.
- **A per-compile opt-in to 16-bit types**: passing the define `AVER_ENABLE_16BIT_TYPES` adds
  `-enable-16bit-types` for that compile alone. `SharcPackedData` uses `float16_t4` and cannot
  compile without it — but enabling it globally would change what `half` MEANS in 93 existing uses
  across water, the material prelude and the path tracer's denoiser, from widened fp32 to genuine
  fp16. A renderer-wide numerical change with no compile error to announce it.

**Measured on the development machine (AMD Radeon RX 7800 XT):**

| | |
|---|---|
| Device shader model | **6.6** |
| Engine compiles shaders at | **6.5** |
| `64-bit shader atomics` | **yes** |

**Those first two rows disagree, and SHaRC reads the wrong one.** Its
`SHARC_ENABLE_64_BIT_ATOMICS` auto-detect keys off the DXC shader TARGET macros
(`SharcCommon.h:96-113`): at SM 6.5 it resolves to 0, meaning "no native atomics, use the software
spin-lock", which needs a fourth buffer — 16 MiB at the suggested 2²² cache size — that this
hardware does not need. **Set the define explicitly from `caps.shaderInt64Atomics`; do not let the
SDK guess.**

**Not done, and not attempted:** the SHaRC Update pass. It is structurally a multi-bounce
path-tracer inner loop (`SHARC_PROPAGATION_DEPTH`, default 4) with no precedent in this codebase —
much closer in shape to `modules/render.pt`'s `PtSceneView` than to Voxi's single-candidate-ray
ReSTIR scheme. That is new algorithmic work plus GPU-hours of parameter tuning, not a wiring
exercise, and SHaRC's own guide devotes a section to the tuning.

**Two SDK facts worth keeping, both of which would cost an implementer time:**

- **`SHARC_QUERY` does not exist.** `Integration.md` calls it required; it appears zero times in all
  four headers. It is a host permutation-naming convention, not a macro branch.
- **`Integration.md` is stale against the vendored v1.6.5.** `SHARC_SAMPLE_NUM_BIT_NUM`,
  `SHARC_SAMPLE_NUM_MULTIPLIER`, `SHARC_RADIANCE_SCALE` and two debug functions it documents are all
  gone; `GetVoxelSize()` is really `HashGridGetVoxelSize()`. Trust the headers, not the guide.
