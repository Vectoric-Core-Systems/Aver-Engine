# Aver.Occlusion  (`modules/occlusion`)

- **Language:** C++
- **Depends on:** Core, RHI (generic only — never a backend)
- **Status:** implemented, flag-gated, default OFF (`--occlusion-cull` in the sandbox)

Hierarchical-Z occlusion culling: a compute-built depth pyramid, and a batch test that answers "did
everything drawn so far already cover this box's screen footprint with something closer?" for a list
of world-space AABBs. See [Occlusion.hpp](include/aver/occlusion/Occlusion.hpp) for the full design —
why this is its own module, why the pyramid reduction is `max()`, why culling has to be two-pass
within one frame, and the one synchronisation cost the design does not hide.

## What it does not know

Nothing about entities, meshes, chunks or clusters. The public interface is `IOcclusionCuller`:
`ensureSized` / `buildPyramid` / `testBatch`, over plain `Aabb{min,max}` structs. `sandbox/src/
SandboxApp.cpp`'s per-entity scene walk is the one caller today; a landscape-tile or cluster culler
is free to become a second one without this module changing.

## The pure half is checkable with no GPU

[OcclusionMath.hpp](include/aver/occlusion/OcclusionMath.hpp) is a header-only, RHI-free re-statement
of the same three steps `OcclusionCuller.cpp`'s `CSTest` compute kernel runs on the GPU: project a
box, pick a conservative pyramid mip, compare against up to four sampled texels. `tests/occlusion`
checks it against synthetic pyramids built by hand, so the algorithm's correctness is settled by
arithmetic before it ever needs a screenshot.

## Turning it off

`--occlusion-cull` (sandbox) is the only thing that turns this on at runtime, default false, exactly
like `--depth-prepass` and `--edge-aa`. `-DAVER_MODULE_OCCLUSION=OFF` removes the module from the
build entirely — the engine still configures, builds and runs, same as any other optional module.
