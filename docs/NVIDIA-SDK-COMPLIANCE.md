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

### 2.2 Attribution and NVIDIA Marks — **PARTLY MET, deliberately**

> "you must attribute the use of the applicable SDK and include the NVIDIA Marks on splash screens,
> in the about box of the application (if present), and in credits for game applications."
> — `third_party/nrd/LICENSE.txt:316-318`

**The decision taken (2026-09-21): we do not use NVIDIA Marks.** The NVIDIA name appears as plain
text identifying the SDKs in use; no trademark, logo or brand asset is reproduced. `LICENSE.md`
states this explicitly rather than leaving it implied.

The reason is that Mark usage is separately gated. Under the same licence, style, colour and
typeface must follow NVIDIA's specifications, and a sample use must be **submitted to NVIDIA for
prior written approval** — with a notice period, and with NVIDIA's silence past ten business days
meaning *deemed unapproved*. Text attribution needs none of that.

**Open, and honest about it:** the clause says "the NVIDIA Marks", not "the NVIDIA name". Plain-text
attribution plainly satisfies the *attribute the use of the applicable SDK* half. Whether it
satisfies the *include the NVIDIA Marks* half is a question this document does not answer. If the
answer must be yes, the approval process above is the route.

**Still open:** the editor's About box (`sandbox/src/SandboxShell.cpp`, `drawAboutPrompt`) does not
yet name NVIDIA or the SDKs. The clause names the about box specifically.

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

---

## 4. Re-running these checks

None of this needs a build or a running engine. From the repo root:

```bash
python scripts/module-guard-audit.py            # unrelated, but the same "check, don't assume" habit
grep -ci nvidia scripts/stage-payload.ps1 scripts/stage-game.ps1   # expect > 0 once §3 is closed
grep -ci nvidia sandbox/src/SandboxShell.cpp                        # expect > 0 once §2.2 is closed
grep -c NVIDIA LICENSE.md                                           # expect > 0 (currently satisfied)
grep -rl "rtxgi\|sharc" --include=CMakeLists.txt --include=*.hlsl . # expect empty while §5 holds
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
