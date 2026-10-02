# NVIDIA SDK usage and licence compliance

**Last audited: 2026-09-25.** Against commit `aca451dd` plus the working tree at that date (first
audit 2026-09-21, `a05275e2`).

**Updated on the change that removed RTXDI and RTXGI.** NRD is now the only NVIDIA SDK vendored. The
earlier audit text about RTXDI and RTXGI (SHaRC) is gone because those SDKs are no longer in the
tree; the dates above refer to the audit of what remains.

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
| **MathLib** | `third_party/mathlib` | Header-only, via NRD | Yes | Yes | NRD's dependency |
| **ShaderMake** | `third_party/shadermake` | Build-time tool | No | No | Compiles NRD's shaders during the build |

**RTXDI and RTXGI were removed on this change.** `third_party/rtxdi` and `third_party/rtxgi` are
deleted. ReSTIR GI is now Aver's own clean-room code (`modules/render.voxi/shaders/voxi_reservoir.hlsli`
and `voxi_restir.hlsli`), written from the published papers (Talbot 2005, Bitterli 2020, Ouyang 2021,
Lin 2022) and not derived from RTXDI source. **No NVIDIA HLSL source ships in `bin/shaders` any more.**
`modules/render.voxi/CMakeLists.txt` deletes stale `bin/shaders/Rtxdi` and `bin/shaders/Sharc`
directories left in old build trees, so a rebuilt tree cannot carry the old copies forward.

**What NRD is used for.** NRD denoises the ray-traced global-illumination signal
(`Voxi NRD denoise` in the per-pass GPU breakdown). It ships as compiled object code only.

---

## 2. The obligations, and where we stand

The licence text lives at `third_party/nrd/LICENSE.txt`. It is the "NVIDIA RTX SDKs License" with an
"NVIDIA RTX SUPPLEMENT" appended; the supplement governs where the two conflict.

### 2.1 The source notice — **MET (source distribution only)**

> "The following notice shall be included in modifications and derivative works of source code
> distributed: *This software contains source code provided by NVIDIA Corporation.*"
> — `third_party/nrd/LICENSE.txt:41-43`

The notice is reproduced verbatim in `LICENSE.md` under **NVIDIA SOFTWARE DEVELOPMENT KITS**.

**Packages (since 2026-09-25):** `THIRD-PARTY-NOTICES.txt` in every staged payload and packaged
game opens its NVIDIA section with this sentence. See §3.

### 2.2 Attribution — **MET, by not having a credit screen**

**Read clause 6.1(c), not 6.1(b).** There are two trademark-placement clauses and they cover
different SDKs. Getting this wrong once already cost an afternoon's worth of the wrong plan.

> **(b)** "NVIDIA Trademark Placement in Applications with the DLSS SDK or NGX SDK. For
> applications that incorporate the DLSS SDK or NGX SDK or portions thereof, you must attribute the
> use of the applicable SDK and include the NVIDIA Marks on splash screens, in the about box of the
> application (if present), and in credits for game applications."
> — `third_party/nrd/LICENSE.txt:313-318`

That is the clause naming splash screens and about boxes, and **it does not apply to this product**,
because neither the DLSS SDK nor the NGX SDK is included (§2.6). The clause that governs NRD is
the next one:

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

Aver Engine is a game engine; NRD is one component of its renderer.

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

### 2.7 MathLib and ShaderMake (MIT) — **MET**

Both are NVIDIA-authored and MIT-licensed. MIT requires the copyright notice and the permission
notice to accompany redistribution. Their texts are intact in their directories and are listed in
`LICENSE.md`. MathLib's licence is reproduced in packaged `THIRD-PARTY-NOTICES.txt` whenever NRD is
compiled in (§3). ShaderMake is not listed there, correctly: it only runs at build time to compile
NRD's shaders, and none of it is redistributed.

---

## 3. Packaged notices — **CLOSED 2026-09-25**

**Until 2026-09-25, no packaged build shipped an NVIDIA notice.** `scripts/stage-payload.ps1` and
`scripts/stage-game.ps1` both built a `THIRD-PARTY-NOTICES.txt`, and neither mentioned NVIDIA, NRD,
MathLib or ShaderMake. `LICENSE.md` covers the **source repository** and does not travel with
a built game.

**Closed by `scripts/NvidiaNotices.ps1`**, which both staging scripts dot-source. Its
`Add-AverNvidiaNotices` appends one NVIDIA section to `THIRD-PARTY-NOTICES.txt`:

1. The §2.1 notice, verbatim: *This software contains source code provided by NVIDIA Corporation.*
2. The §2.2 attribution: a plain-text list of the NVIDIA SDKs the package uses, and how each is
   present. For a package, this file is the "end user documentation" that clause 6.1(c) routes
   attribution to. No NVIDIA Marks are used.
3. Each shipped component's licence, reproduced in full from its vendored file.

**It lists only what ships.** A notice for an absent component is as wrong as a missing one:

- **NRD and MathLib** are listed when `AVER_WITH_NRD` and `AVER_MODULE_VOXI` are both ON in
  `CMakeCache.txt`. NRD is compiled into the renderer, and MathLib is compiled in with it.

`NvidiaNotices.ps1` lists NRD only. A missing licence file is a hard `Fail`, as it is for every
other entry in the stagers' `$components` lists.

**Verified 2026-09-25** by calling the helper directly against `build-release/bin`, with no staging
run. At that date it also listed RTXDI and SHaRC, which have since been removed; the NRD
and MathLib entries are unchanged. All three scripts parse cleanly.

### 3.1 Packaging is STILL BLOCKED — lifting it is the owner's call

**§3 is closed, but the gate below is deliberately still in place.** Removing it is a separate
decision, not a side effect of adding the notices. Until someone makes that call:

- Staging still needs `-IAcceptNvidiaRedistribution`.
- The gate's log lines no longer claim the package is built "WITHOUT the third-party notices" --
  that stale wording (both stagers' blocked-exit message and their `-IAcceptNvidiaRedistribution`
  log line) was corrected alongside this section on 2026-09-25. The notices are written either way;
  what `-IAcceptNvidiaRedistribution` skips is the owner's sign-off, not the notices themselves.

**Decision, 2026-09-21: rather than produce packages that are not clear to distribute, the
packaging path refuses.** The engine is in beta and nothing is shipping, so stopping costs nothing
and is the honest answer; producing a package that quietly omits required notices would not be.

Both `scripts/stage-game.ps1` and `scripts/stage-payload.ps1` exit 2 with the reason before doing
any work. That covers the editor's **File ▸ Package Project…** too, since it shells out to
`stage-game.ps1` rather than staging anything itself.

`stage-payload.ps1` is gated as well as `stage-game.ps1`, and deliberately: it stages the ENGINE for
a developer rather than a GAME for a player, but it redistributes the same compiled NRD, so the same
obligation attaches to it.

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
grep -c Add-AverNvidiaNotices scripts/stage-payload.ps1 scripts/stage-game.ps1   # expect 1 each (§3)
# NOT a check: the About box is deliberately NOT an attribution surface -- see §2.2.
grep -c NVIDIA LICENSE.md                                           # expect > 0 (currently satisfied)
grep -rn "include.*\(Rtxdi\|Sharc\)/" --include=*.hlsl --include=*.hlsli modules   # expect empty (§1, §5)
ls bin/shaders/Rtxdi bin/shaders/Sharc 2>&1                          # expect "No such file" (CMake removes them)
```

To see the NVIDIA section a package would carry, call the helper directly (no staging needed):

```powershell
function Fail([string] $m) { Write-Host "FAIL $m" }; . ./scripts/NvidiaNotices.ps1
$o = @{}; Get-Content build-release/CMakeCache.txt | % { if ($_ -match '^(AVER_[A-Z0-9_]+):BOOL=(ON|OFF)$') { $o[$Matches[1]] = $Matches[2] -eq 'ON' } }
$sb = [Text.StringBuilder]::new(); Add-AverNvidiaNotices $sb (Get-Location) build-release/bin $o; $sb.ToString()
```

And the block itself, which should refuse with exit 2 until §3 is closed:

```bash
pwsh ./scripts/stage-game.ps1 -Project <any.ocproject> -Out <tmp>   # expect: PACKAGING BLOCKED, exit 2
```

**When the vendored SDKs are updated, re-read the licence texts rather than assuming they are
unchanged.** NVIDIA has revised these licence documents between SDK drops in the past (formatting and which
SDKs the preamble enumerates), so a new NRD drop may carry different terms.

---

## 5. Patents and provenance of the in-house ReSTIR GI

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
