# abi/

**Empty by decision. There is no consolidated Aver.ABI and there is not going to be one.**

This directory once described a single flat `extern "C"` interop seam that everything non-C++ would
bind against, planned for "Phase 7". That plan has been dropped. The per-module seams stay separate,
and they are the architecture rather than a step towards something else.

Separate is what buys the properties the seams exist for. `Aver.Scene` can be read end to end without
meeting the word *actor*, enforced by a link line rather than by discipline. `Aver.Render.PBR` can be
a P/Invoke DLL with no RHI type anywhere behind it. Each module versions on its own, so the
framework's surface can move while the scene's is still settling. One flat seam would have had to
give up all three to gain a single version number nobody was asking for.

**The seams are documented together in [../docs/ABI.md](../docs/ABI.md)** — which one to reach for
given the job you have, every entry point with its signature, the type and handle rules, the three
version boundaries, and the checklist for adding an entry point.

Nothing here is built. `../interop/` is empty for the same reason: the C# bindings under
`../scripting/csharp/` are hand-written rather than generated, and there is no Rust in this tree.
