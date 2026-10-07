# Tooling comparison: Drift Engine

Compared 2026-10-08 against [drftrun/driftengine](https://github.com/drftrun/driftengine) (Apache-2.0, TypeScript,
WebGPU/WebGL2), read through the GitHub API at header and doc level. Ideas are free to reimplement; copying Drift code
or text needs its Apache-2.0 header and a NOTICE entry (avoid `demo/dev/public/fonts` and `packages/native-host`
licences entirely).

## Drift's approach in one paragraph
Every claim gets an executable check (~70 `scripts/*-check.mjs`, ~90 single-feature `demo/dev/*.html` pages), and the
checks themselves are mutation-tested (`claimAudit.sh` deletes the measured term and expects the check to fail).
Pixel gates hold a virtual clock (`heldFrame.ts`) so two runs of one build match exactly, and `shots.mjs` diffs every
scene before/after with island clustering. Frame cost is counted (passes, bytes moved) rather than timed. Generated
artifacts (shaders, capability registry, manual code blocks, size floors) are committed and checked for staleness.

## Gaps worth closing (ranked by value / effort)
| # | Drift tool | Aver today | Proposal | Effort |
|---|---|---|---|---|
| 1 | `scripts/claimAudit.sh` | none | `scripts/claim-audit.ps1 <gate|ctest> <file> <old> <new>`: apply one edit, rebuild, run the check, report caught/survived; MCP tool `aver_claim_audit` | S-M |
| 2 | `heldFrame.ts` + `shots.mjs` | `gates.ps1` sparse probes, `--screenshot` | `scripts/shots.py capture|diff`: level/camera list -> PNGs; diff reports mean/p99/max delta and connected islands above the run-to-run noise floor | M |
| 3 | `AGENTS.md` | rules live outside the repo | a checked-in agent guide: gate costs and order, "never delete build\\", which checks need a GPU, DRED/stderr capture | S |
| 4 | `frame-audit.mjs` | timing only (`--gpu-timing`) | RHI counters (passes, barriers, copies, bytes read/written) behind `--frame-audit`, shown in the Profiler panel | M |
| 5 | `gpu-parity.mjs`, `inference-parity.mjs` | `--nrd2-oracle`, `--furnace-test` | GPU-labelled CTests that dispatch a pass on tiny inputs and compare with its C++ reference | M |
| 6 | `manual-sync`, `docs:check` | hand-written docs (`docs/STALE_CODE.md`) | `scripts/docs-check.py`: code blocks tagged with a source region fail when the region changes; flag table generated from `aver_flags` | M |
| 7 | `drft-diff`, `bake:check` | none | `tools/OcmeshDiff.cpp`: compare two `.ocmesh` by meaning (counts, materials, LODs, bounds); CTest bakes twice and diffs | S |
| 8 | `probe-check.mjs`, `packages/tools` overlay | `ShaderWarmup`, editor profiler | `--shader-report` (compile cost per shader, refusals under `--force-caps`); an F3 stats overlay in `Runtime/` | S-M |

Smaller: per-artifact size floors next to `verify-payload.ps1`; a static determinism scan for simulation code;
auto-captured example stills; generated changelog; a lint step in `.github/workflows/ci.yml` (which has not run yet).

## Where Aver is ahead
MCP server for agents (`tools/mcp/aver_mcp.py`: build, run, capture, gates, live editor control); render oracle across
Debug/Release, Vulkan and degraded caps (`scripts/gates.ps1`); a much deeper native editor; a production BC7 encoder
with a derived-data cache; native diagnostics (`--dred`, `--gpu-validation`, `--device-lost-at`, crash reporter,
error-code table); module-matrix and guard audits; comment-only edit proof (`scripts/check-code-unchanged.py`);
neural-renderer training tools.
