# Aver.Physics.Jolt  (`modules/physics.jolt`) — the rigid-body backend, vendored

- **Language:** C++ (vendored, not authored here)
- **Depends on:** nothing in this tree
- **Consumed by:** `Aver.Physics` (`PRIVATE`), and `tests/physics` alone

Jolt is the concrete backend behind [`Aver.Physics`](../physics/README.md)'s plain-C ABI, and this
directory is named the way every other backend in the tree is named: `rhi` + `rhi.d3d12`, `audio` +
`audio.wasapi`, `formats` + `formats.roslyn`, `physics` + `physics.jolt`. The generic module states
the contract; the dotted one satisfies it. That is a claim about the module graph — physics is not a
dependency the build happens to pull in, it is a subsystem of the engine — and it is enforced on a
link line rather than asserted: `Aver.Physics` links this `PRIVATE`, so no `JPH::` header can reach
anything above.

**It moved out of `third_party/` in July 2026 and NOTHING else about it changed.** The sources are
still upstream's, still not ours to edit, and still MIT — the provenance table below, the `LICENSE`
beside this file, the `/W0`, and the update procedure at the bottom are all exactly as they were. If
you are here to change a file under `Jolt/`, the answer is still no: see *Updating*.

The target the vendored `Jolt/Jolt.cmake` creates is called `Jolt`, because that file is upstream's
and editing it would be undone by the next update. `CMakeLists.txt` here adds an
`add_library(Aver.Physics.Jolt ALIAS Jolt)` so consumers' link lines read like every other module's.

## Provenance

| | |
|---|---|
| **Version** | 5.6.0 (`JPH_VERSION_MAJOR/MINOR/PATCH` in `Jolt/Core/Core.h`) |
| **Upstream** | <https://github.com/jrouwe/JoltPhysics> |
| **Source archive** | `https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.zip` |
| **SHA-256 of that archive** | `0AF9BEEA51637EF805E624FE838EA2870F7B68CD48CBE1615C853BD9BCF4F1D7` |
| **Licence** | **MIT**, © Jorrit Rouwe — see `LICENSE`. Verified from the upstream `LICENSE` file: no fee, no revenue threshold, no per-title registration, no commercial tier. |
| **Vendored** | July 2026, as `third_party/JoltPhysics`; moved here July 2026 with the sources untouched |

`scripts/stage-payload.ps1` concatenates the `LICENSE` beside this file into
`THIRD-PARTY-NOTICES.txt`, by a repo-relative path that names this directory. The MIT copyright
notice is mandatory in a shipped build, so that script **fails** rather than warns if the file is not
where it expects — which is what stops this obligation being quietly dropped by a rename.

## What is here, and what is not

Only the `Jolt/` library directory (560 files, 4.3 MB; 153 `.cpp`) plus `LICENSE`. Upstream's
`Samples/`, `TestFramework/`, `JoltViewer/`, `PerformanceTest/`, `UnitTests/`, `HelloWorld/`,
`Assets/`, `Docs/` and `Build/` are **not** vendored — the engine builds none of them.

Because `Build/` is absent, the `option()` declarations `Jolt/Jolt.cmake` reads are declared by the
`CMakeLists.txt` here instead. That file documents each choice; the two that would be surprising:

- **No AVX/AVX2, no FMADD.** AVX2 would raise the whole engine's minimum CPU to Haswell, and FMADD
  changes floating-point rounding — which this repo verifies against a pixel-exact baseline.
- **`ENABLE_OBJECT_STREAM=OFF`**, since the engine serialises through `.ocmap`, not Jolt's format.

A consequence of not vendoring `Build/` worth recording: upstream sets warnings-as-errors and
disables C++ exceptions and RTTI *there*, not in `Jolt.cmake`. So none of that reaches this build,
which is why the engine can keep using `try`/`catch` around `std::filesystem` with Jolt linked in.

## Conventions — Jolt does not share the engine's

This is the thing to remember when touching the physics module. Jolt's own docs
(`Docs/Architecture.md` upstream) state: *"Jolt Physics uses a right handed coordinate system with
Y-up"*, *"We use column-major vectors and matrices"*, and *"the physics simulation works best if you
use SI units (meters, radians, seconds, kg)"* — with accuracy assuming dynamic bodies in
`[0.1, 10] m` and gravity in `[0, 10] m/s²`, plus an explicit note to *"consider scaling the objects
before passing them on to the physics simulation"* if you use different units.

| | Jolt | Aver |
|---|---|---|
| Handedness | right | **left** |
| Up axis | +Y | **+Z** |
| Matrices | column-major, `M * point` | **row**-major, row-vector, translation in the last row |
| Units | metres | **centimetres** |

All four differ. Conversion is therefore mandatory at the boundary and is **not** a relabelling:
feeding centimetres in unchanged would place a 160 cm character at "160 m" and gravity at 980, both
outside the range Jolt documents as accurate. The conversion lives in one place in the physics
module, with round-trip tests, precisely so it is never done ad hoc at a call site.

## Determinism

Upstream: the simulation is deterministic *"provided that the APIs that modify the simulation are
called in exactly the same order"* and *"the same binary code is used"*. Two documented exceptions
that matter for any oracle gate built on physics:

- **Broadphase queries are not deterministic** — the broad phase can be modified from several threads.
- **Listener callback order is not deterministic**, and narrow-phase query *result order* can vary
  even though the results themselves are consistent.

So a gate may assert on simulated state, but must not depend on the order in which query results or
callbacks arrive.

## Updating

Replace `Jolt/` and `LICENSE` from a fresh archive, update the version and SHA-256 above, and re-run
the conversion round-trip tests before trusting anything. Do not hand-edit the vendored sources — a
local fix here is invisible at the next update.
