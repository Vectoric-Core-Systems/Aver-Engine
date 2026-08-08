# Visual scripting: feasibility, and a plan

> **STATUS: PLAN ONLY. No code has been written.** Produced 2026-08-08 from a feasibility survey
> (six readers over the scripting stack, the ABI surface, the editor UI, the formats, the frame loop
> and the constraints) followed by a design pass over the four areas that decide the cost.
>
> Two claims the agents produced are **corrected below rather than carried forward**. Both are
> flagged in place. Re-check anything load-bearing before relying on it.

---

## 1. The finding that reorders everything

**The graph canvas is not the expensive part. The node glue is.**

There are 207+ hand-written `extern "C"` ABI entry points across seven headers, and **no reflective
by-name call mechanism anywhere** — `abi/README.md:19-20` states the C# bindings are hand-written
rather than generated, and `docs/ABI.md`'s own checklist for adding an entry point is entirely
manual. So every node that calls engine functionality needs hand-written marshalling.

A plan whose first slices are all UI is therefore avoiding the risky part. The canvas is roughly
500 lines of solved ImGui work; the glue is the unknown. **If the glue is intolerable, a beautiful
canvas is a tool for nothing.**

### The one lever that matters

`modules/scene` has a complete, tested, name-indexed **component field registry** — `FieldDesc` with
name, component id, kind, offset and a read-only flag (`Fields.hpp:42-51`), every built-in component
registered through `ComponentBuilder` (`Builtins.cpp:28-113`), and the same public API that
script-declared components use.

That collapses what would be **80+ hand-written per-field nodes** (get/set health, ammo, mesh,
material, …) into exactly **two generic nodes**: `GetComponentField(entity, fieldName)` and
`SetComponentField(entity, fieldName, value)`, resolving the field id once at bind time.

This is the single biggest lever on the cost, and it is the reason the answer is "feasible" rather
than "not worth it". It covers a graph's **data** side. It does not cover the **call** side — spawn,
destroy, possess, raycast and friends still need one wrapper each, and about **25** of them buy
roughly 80% of gameplay scripting.

---

## 2. Two corrections to the agents' own output

**The Scene API is not missing.** The feasibility synthesis claimed "the Scene API (object spawn,
physics, components) is not implemented … only material/rendering scripting works today". **That is
false.** `tests/scene` has 538 passing assertions, `tests/framework` 228; actors spawn, possess and
tick; the physics ABI creates bodies and raycasts. The claim appears to come from reading
`docs/STATUS.md` rather than the code, and the study's *own* survey agent contradicted it. It was
corrected before it could shape the plan.

**The native interpreter is not mandatory.** The plan called a native fallback interpreter "high
risk" and "mandatory for non-Windows support", on the grounds that the CLR host is Windows-only.
The CLR host *is* Windows-only — but so is the engine. `modules/platform/src/win32/Win32Window.cpp`
carries **zero** `#if defined(_WIN32)` guards and is listed unconditionally in the platform module's
`SOURCES`, so a non-Windows build does not fail at the script host; it fails at the window.

> **Therefore the CLR's Windows-only limitation adds no constraint the engine does not already
> have**, and building a second executor for a platform that cannot compile the platform layer is
> speculative work. Slice 4 below is **deferred**, not mandatory. Revisit it when — and only when —
> the engine itself becomes cross-platform, at which point the graph *format* is already portable
> and only the backend needs writing.

That correction removes the highest-risk slice in the plan.

---

## 3. Decisions

| Decision | Consequence |
|---|---|
| **Prove the glue before building the canvas.** | Slice 0 is five nodes and no UI at all. If it is tedious, the plan dies in a week instead of after the canvas. |
| **Two generic field nodes over the field registry**, plus ~25 hand-written ABI wrappers. | Removes 80+ nodes of repetition. The registry is already tested, so this leans on something real. |
| **No code generation from headers.** | P/Invoke signatures are stable once written; a header parser is a fragile step that buys nothing. Write them once, by hand, with tests. |
| **No reflective by-name call mechanism.** | 207 fixed entry points do not need one, and adding one is complexity plus cost for a problem that does not exist. |
| **`.ocgraph` is TEXT**, in the `OC` dialect (`OCGRAPH 1`, then records). | Diffable, mergeable, hand-editable — the same reasons `.ocworld` is text. |
| **Custom ImGui canvas**, not a vendored node library. | ~500 lines, full schema control, and no new dependency under the permissive-only rule. Vendor later if maintenance exceeds integration. |
| **Compile in the editor, never at runtime.** | A shipped game has no compiler. Assets package the compiled form. |
| **Windows-first, and said out loud.** | Execution is Roslyn compile-to-C# through the existing escalation path. See §2 for why that is not the limitation it looks like. |

### The trap `.ocworld` already fell into

`OcWorld`'s parser **skips unknown records**, and the editor **rebuilds the file from its own
state** — so anything the editor does not model is silently dropped on the next save. `PCGVOLUME`
hit exactly this and is worked around by stashing the records verbatim at load
(`SandboxApp.cpp:5478`). A graph format must carry unknown records through by construction, not by
remembering to.

---

## 4. Slices

**Slice 0 — five nodes, no UI.** `Aver.Scripting.Nodes` with `GetComponentField`, `SetComponentField`,
`SpawnActor`, `DestroyActor`, `Raycast`, each a hand-written P/Invoke over the existing ABI.
*Done when:* `GetComponentField(entity, "CLocal.position")` returns a float matching the entity's
actual position to six decimals, `SetComponentField`'s write is visible through `aver_scene_field`,
and `SpawnActor` returns a valid handle. **This slice decides the plan.** If five nodes take three
weeks instead of one, the glue hypothesis is wrong and the architecture is reconsidered.

**Slice 1 — the `.ocgraph` format.** `parseOcgraph`/`serializeOcgraph`, asset type, content browser.
*Done when:* ten graphs (empty, single node, branching, cyclic, every pin type) round-trip
**bit-identically**, and injected unknown records survive a parse-serialise cycle rather than being
dropped.

**Slice 2 — the canvas.** An `AssetEditor` subclass: nodes, pins, bezier links, pan/zoom, selection,
connection, and undo on the editor's existing stack.
*Done when:* a hand-authored 5-node graph saves and reopens with layout preserved to ±10 px, and a
20-node graph renders without hitching.

**Slice 3 — execution, Roslyn backend.** Graph → C# source → the existing two-tier compile path →
a collectible ALC, dispatched through the framework's existing table.
*Done when:* a graph that spawns an actor, reads a field, modifies it and destroys it produces the
same entity state as the equivalent hand-written C# actor.

**Slice 4 — native interpreter. DEFERRED, not mandatory.** See §2. Revisit when the engine is
cross-platform; the format is portable already, so only the backend is owed.

**Slice 5 — the other ~20 ABI wrappers.** Framework, physics, PBR.
*Done when:* ≥25 node classes with unit tests, and a 10-node multi-branch graph executes.

**Slice 6 — node discovery.** A registry driving the Add-Node menu and pin type validation.
*Done when:* invalid connections are refused (five mismatched pairs tested), and read-only fields
are shown as read-only rather than silently failing — the ABI returns 0 on a read-only write, and a
user must not read that as "it worked".

**Slice 7 — hot reload.** Graphs through the `ScriptHost` unload/load cycle.
*Done when:* an edited graph is live after reload with no restart, and in-flight ticks do not
deadlock. Note `HostBridge`'s unload is a **request**, not a guarantee — a graph holding a reference
into the old ALC prevents collection.

---

## 5. What I would not do

1. **Do not build the canvas first.** It inverts risk against value.
2. **Do not vendor a node-graph library yet.** The permissive-only rule limits the field, and ~500
   lines of ImGui keeps schema control. Revisit when maintenance exceeds integration.
3. **Do not generate P/Invoke from headers.** A fragile parsing step for a one-time cost.
4. **Do not invent reflective by-name calling.**
5. **Do not build the native interpreter now** (§2).
6. **Do not support nested subgraphs in v1.** Flat dataflow compiles, debugs and versions far more
   simply; separate `.ocgraph` files are enough reuse to start.
7. **Do not compile at runtime.** A shipped game has no compiler.
8. **Do not hide read-only fields from `SetComponentField`.** Show the failure; a silent no-op reads
   as a bug in the graph.
9. **Do not add a hook to the framework dispatch table without budgeting for it.** It is exactly ten
   entries; adding one bumps `AVER_FW_DISPATCH_VERSION` and breaks the managed binding until it is
   regenerated.
