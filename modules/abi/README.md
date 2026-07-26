# Aver.ABI  (`modules/abi`) — dropped

**This module does not exist and will not be built.** It is kept as a directory only so that anyone
who finds it in the module map, or in an old commit, learns the decision instead of re-proposing it.

It described a single flat `extern "C"` seam — opaque handles, out-pointer returns, an
`aver_abi_version()` — as "the single interop boundary C#/Rust bind against", to be wired into the
top-level `CMakeLists.txt` at Phase 7. The seams stay **separate** instead. That is the architecture,
not an interim state.

The reason is that consolidating would cost the three properties the separate seams are for:

- `Aver.Scene` carries no gameplay vocabulary, and that is enforced by a link line — it does not link
  `Aver.Framework` — rather than by discipline.
- `Aver.Render.PBR` is a P/Invoke DLL with no RHI type anywhere in its transitive closure.
- Each module versions independently, so one surface can move while another is still settling.

A single seam has to link everything it exposes, so it could hold none of them.

**Where to look instead:** [docs/ABI.md](../../docs/ABI.md) documents all six seams in one place —
which to use for a given job, every entry point and signature, the type and handle rules, the error
convention, the three version boundaries, and what must change together when you add an entry point.

`docs/ARCHITECTURE.md` still carries the old sketch in several places and is stale on this point.
