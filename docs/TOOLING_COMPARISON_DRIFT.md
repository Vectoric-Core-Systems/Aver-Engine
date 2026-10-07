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
| 4 | `frame-audit.mjs` | timing only (`--gpu-timing`) | **DONE.** `AVER_FRAME_AUDIT=1` makes the D3D12 backend count draws, dispatches, barriers, copies (and bytes), clears, PSO binds and submits per GPU timing span; see below | M |
| 5 | `gpu-parity.mjs`, `inference-parity.mjs` | `--nrd2-oracle`, `--furnace-test` | GPU-labelled CTests that dispatch a pass on tiny inputs and compare with its C++ reference | M |
| 6 | `manual-sync`, `docs:check` | hand-written docs (`docs/STALE_CODE.md`) | `scripts/docs-check.py`: code blocks tagged with a source region fail when the region changes; flag table generated from `aver_flags` | M |
| 7 | `drft-diff`, `bake:check` | none | `tools/OcmeshDiff.cpp`: compare two `.ocmesh` by meaning (counts, materials, LODs, bounds); CTest bakes twice and diffs | S |
| 8 | `probe-check.mjs`, `packages/tools` overlay | `ShaderWarmup`, editor profiler | `--shader-report` (compile cost per shader, refusals under `--force-caps`); an F3 stats overlay in `Runtime/` | S-M |

Smaller: per-artifact size floors next to `verify-payload.ps1`; a static determinism scan for simulation code;
auto-captured example stills; generated changelog; a lint step in `.github/workflows/ci.yml` (which has not run yet).

## Frame audit (gap 4, done)
Counting, not timing: how much work a frame asks of the GPU, which does not move with the machine, the clocks or the
driver, so two runs of one scene print the same numbers and a change that adds a pass, a barrier or a copy shows up as a
diff in a log instead of a noisy millisecond.

Use: set `AVER_FRAME_AUDIT=1` in the environment of any host that creates the D3D12 device (editor, runtime, game). It is
read once at device creation; unset, each counted call costs one never-taken branch. Every 128 frames, at the start of the
next frame (where the `[RHI.D3D12] GPU ...ms/frame` timing tree is logged), the window is printed as `[Audit]` lines:

    [Audit] per-frame counts over 128 frames (...); window totals: draw N cs N ms N rt N bar N cpy N bytes N clr N pso N exec N
    [Audit]   ALL  draw 41.00  cs 96.00  ms 0.00  rt 2.00  bar 180.00  cpy 3.00 (12.500 MB)  clr 6.00  pso 130.00  exec 1.00
    [Audit]     scene draw  draw ...
    [Audit]       RD ...                      (children indented, same names and nesting as the timing tree)

Columns are per-frame averages over the window: `draw` (DrawInstanced and DrawIndexedInstanced calls), `cs` / `ms`
(compute and mesh-shader dispatches), `rt` (acceleration-structure builds; the engine uses inline ray queries, so there
is no DispatchRays), `bar` (individual barriers, a 3-barrier call counts 3), `cpy` and its MB (CopyResource,
CopyBufferRegion, CopyTextureRegion and MSAA resolves; bytes are tight pixel/byte counts of what moves), `clr` (render
target and depth clears), `pso` (SetPipelineState calls actually issued), `exec` (ExecuteCommandLists calls). A span
line includes its children; work outside any span is `(outside spans)`. Present-thread copies and the one-shot upload and
readback lists are shown as `(present thread)` and `(uploads and readbacks)`. The `window totals` integers on the header
line are exact; compare those (or whole `[Audit]` blocks) between two logs. Windows that overlap level loading differ
from steady state, so compare the same window number. The present thread runs asynchronously, so its line can shift
by one present across a window boundary; the render-thread lines do not.

Checked 2026-10-08: two runs of NewSponza_Night (`--denoiser 2`, 600 frames) printed identical `[Audit]` blocks in all four
windows.

Limits: D3D12 only for now (the Vulkan backend prints nothing). The counters live in `D3D12Device.cpp` next to the
timing spans (`beginGpuSpan` / `pushMarker`), one hook per recorded command.

## Where Aver is ahead
MCP server for agents (`tools/mcp/aver_mcp.py`: build, run, capture, gates, live editor control); render oracle across
Debug/Release, Vulkan and degraded caps (`scripts/gates.ps1`); a much deeper native editor; a production BC7 encoder
with a derived-data cache; native diagnostics (`--dred`, `--gpu-validation`, `--device-lost-at`, crash reporter,
error-code table); module-matrix and guard audits; comment-only edit proof (`scripts/check-code-unchanged.py`);
neural-renderer training tools.
