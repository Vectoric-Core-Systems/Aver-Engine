# Landscape editor: feasibility, and a plan

> **STATUS: PLAN ONLY. No code has been written.** Produced 2026-08-08 from a five-area survey —
> the CPU model (`modules/landscape`), `.ocland` persistence and quantisation arithmetic, the
> renderer's per-frame contract, editor precedent in `sandbox/src` (asset editors, undo,
> content-browser registration, ray-picking), and physics plus level-reference design — each
> finding carrying a `file:line` an agent actually opened, distilled into the blockers, decisions
> and slices below.

---

## 1. Blockers

Nine blockers stand between this module and an editor. The first four are not just items on a
list — each one reshapes what the plan can look like, and together they are why slice 0 below is
"get terrain on screen and referenced from a level", not a brush.

### 1. Nothing in the engine calls the landscape module *(there is nothing to sculpt against until this closes)*

`Aver.Landscape` and `Aver.Landscape.Renderer` are linked from nowhere but their own
`CMakeLists.txt` and `tests/landscape/CMakeLists.txt:5,23-24,35` — a repo-wide grep for every
`Aver\.Landscape` target confirms it. A second grep, for the symbols themselves (`LandscapeTree`,
`LandscapeRenderer`, `loadOcLand`, `OcLandData`, `buildChunkMesh`), returns exactly 19 files, every
one of them inside `modules/landscape`, `modules/formats`, `tests/landscape`, `tests/formats`, one
RHI comment, or `docs/`. No `.ocland` file exists anywhere in the repo or on disk. **An editor here
is not a UI wrapped around a working module — it is new engineering layered on a headless CPU model
that nothing in the running engine has ever exercised end to end.** Slice 0 therefore has to be
"terrain on screen and referenced from a level", because there is nothing to sculpt against and no
level format that can even point at a section until that path exists.

### 2. `.ocland` silently degrades work the artist never touched *(worse than no editor if left unaddressed)*

`writeOcLand` recomputes `lo`/`hi` — and from them the u16 bias and scale — from whatever is
currently in `OcLandData::heights`, on every single save (`modules/formats/src/OcLand.cpp:121-128`).
Resaving without editing is a fixed point: the samples holding the extrema always round to `q=0` and
`q=65535` exactly, so a second, third or tenth save does not move the grid. But the moment an edit
moves the section's global minimum or maximum — a sculpt session pushing a peak higher is exactly
this — the next save fits a new, coarser grid to the *whole* section, and there is no float value
retained anywhere to recover from it. **A save silently re-quantises every sample in the section,
including ones the artist never touched, and the loss is permanent.** An editor that does this on
every save is worse than no editor at all: it teaches the artist that saving is safe when it is not.

### 3. The renderer will draw stale terrain forever *(the central finding for editing)*

`LandscapeRenderer`'s mesh cache (`meshes_`, an `unordered_map<u32 nodeIndex, Resident>`,
`LandscapeRenderer.hpp:75`) is keyed by a topological node index that `LandscapeTree::build()`
assigns purely from `(level, ny, nx)` — never from height values — so the same index refers to the
same place before and after a rebuild. `draw()` looks the index up and, if present, reuses the
cached mesh unconditionally; it only builds a new one when the index is *absent*
(`LandscapeRenderer.cpp:72-104`). **Nothing checks whether the samples under a cached mesh changed
since it was cached.** After a height edit and a tree rebuild, every resident node whose footprint
overlaps the edit — at every level that has a cached mesh there, not only the level currently
selected — keeps drawing its pre-edit geometry indefinitely. The only lever that exists is
`forgetAll()` (`LandscapeRenderer.hpp:54-58`), which destroys the *entire* resident set through the
real `destroyMesh` added this session: a full-section stall for a one-sample edit, not a fix.

### 4. `LandscapeTree::build()` clears the tree before it validates *(destroys work speculatively)*

`nodes_`, `levelError_`, `refined_`, `rootIndex_` and `levels_` are all cleared unconditionally at
the top of `build()`, before any validation runs; a validation failure then returns early without
repopulating them (`modules/landscape/src/LandscapeTree.cpp:68-84`). **A rejected build leaves the
tree empty, not unchanged.** If an editor ever calls `build()` speculatively — to validate a resize,
say, against the tree a section is currently rendering from — a rejected call destroys the working
tree, not just the attempted change. The module provides no rollback of its own: an editor must
build into a scratch `LandscapeTree` and swap it in only on success.

### 5. `build()` has no incremental or region-scoped rebuild path

The class's entire public surface is `build()`, `select()` and `resetHysteresis()`
(`modules/landscape/include/aver/landscape/LandscapeTree.hpp:76-92`); `build()` always walks every
level and every node of the whole section, re-scanning every source sample for its bounding-sphere
extent and, above level 0, its `errorCm` (`LandscapeTree.cpp:95-159`). There is no "rebuild this
node" or "rebuild this rectangle of samples" entry point. It gets worse one level up: `skirtCm` at
level L is derived from `levelError_[level+1]`, the maximum `errorCm` across every node at the next
coarser level in the *whole* section (`LandscapeTree.cpp:129-139`) — a running max with no way to
un-take a value, so it can only grow locally; shrinking it after a flattening edit needs a full-level
rescan (`LandscapeTree.cpp:165-168`). **A single sculpted vertex requires re-touching essentially the
whole section** — for a 1025-sample production section, upward of a million sample visits across
five levels, per `docs/STATUS.md:2632-2634`'s own production numbers. That is not per-frame-during-
a-drag interactive without new engineering: it has to be debounced to end-of-stroke.

### 6. `physics_abi.h` has no way to patch a heightfield — only replace it whole

The ABI exposes `aver_phys_add_heightfield` (full create) and `aver_phys_remove_body` (full destroy)
and nothing in between. `PhysicsBridge::toPhysicsHeightfield` converts the entire `OcLandData`
section on every call (`modules/landscape/src/PhysicsBridge.cpp:7-30`). The vendored Jolt backend
already has a cheaper primitive — `HeightFieldShape::SetHeights`
(`modules/physics.jolt/Jolt/Physics/Collision/Shape/HeightFieldShape.h:231`), a block-aligned
in-place sub-rectangle update explicitly documented to avoid a full shape rebuild — that the engine's
own ABI never wraps. **Collision feedback while sculpting has the same whole-section-rebuild cost as
the render tree**, even though the underlying library already ships a cheaper path for it.

### 7. `OcWorldData` has no landscape field, and this codebase has already been burned by exactly that gap

`OcWorldData` (`modules/formats/include/aver/formats/OcWorld.hpp:53-88`) carries `SUN`/`FOG`/`SKY`/
`SPAWN`/PCG volumes and placements — nothing landscape-shaped. Its parser skips unrecognised records
silently, and its writer only ever emits fields present on the in-memory struct. The `PCGVOLUME`
incident is the documented case history for what that combination does: `saveLevel` used to build a
fresh `OcWorldData` from editor state and silently dropped every field it didn't model — `SPAWN`
deleted, `BUILD` zeroed, the sun's `lux` reverted — until `levelHeader_` (a captured copy of the
loaded data) and a dedicated `levelPcgVolumes_` vector were added specifically to stop it
(`sandbox/src/SandboxApp.cpp:5464-5526`). **A bare `landscapes` field would ride through passively
only until the editor gains the ability to change what it references — at which point it reproduces
the PCGVOLUME bug verbatim** — so the explicit capture-at-load / clear-from-header / rebuild-at-save
treatment has to land in the same slice as the field itself, not later.

### 8. `sandbox/src` has no plumbing for this asset type at all

No `AssetEditorFactory` claims `.ocland` — the concrete factories registered in `onInit`
(`SandboxApp.cpp:498-501`) are for `.ocmesh`, actor and anim assets only, each checking its own
extension in the style of `makeMeshEditor`'s `if (ext != ".ocmesh") return nullptr;`
(`sandbox/src/AssetEditor.cpp:223`). The content browser's icon lookup (`assetIconTile`, a fixed
three-way `if`-chain, `SandboxApp.cpp:3979-3984`) and its `kAssetIconTiles = 3` sprite sheet have no
fourth entry. And the only ray/pick code in the editor, `pick()` (`SandboxApp.cpp:2903-2922`, with
`rayAabb` defined at `:192`), tests ray-vs-AABB against placeholder objects and mesh-renderer bounds
— never a heightfield. A double-clicked `.ocland` file falls through to the IDE-or-shell path today,
and there is no way to resolve a screen point to a point on the terrain surface. None of this is
wiring an existing but disconnected path; it is new surface from zero.

### 9. Two independent draw-budget ceilings that do not share a budget

`LandscapeTree::SelectParams::maxDraws` (default 192, `LandscapeTree.hpp:61-62`) is enforced inside
`select()` itself, upstream of the renderer entirely, dropping nodes in ascending screen error past
the ceiling; `LandscapeRenderer`'s own `maxResidentNodes` cache (default 512, `LandscapeRenderer.hpp:35,81`)
is a separate, later ceiling. The module's own README sizes the 192 figure against a 1 MiB transient
constant ring assumed to serve *one* section (`modules/landscape/README.md:71-75`). **Two open
sections, each independently under its own 192-node ceiling, can jointly submit up to 192×N nodes
into the same 1 MiB ring** — past exhaustion the constant buffer silently goes unbound and shading is
wrong with one logged error. Tuning either ceiling alone leaves the other free to blow the budget.

---

## 2. `modules/landscape/README.md` is stale — trust `docs/STATUS.md`

The module's own README states "stages 1–2 implemented — the whole CPU model. Nothing is rendered
yet" (`modules/landscape/README.md:5`), and frames the physics transpose as something the bridge
"must" still do, under a heading that reads like unfinished future work — "stage 3". Neither is true
of the tree today. `Aver.Landscape.Renderer` is a real, linked target
(`modules/landscape/CMakeLists.txt:33-40`) with a working eviction cache and real `destroyMesh` calls
(`LandscapeRenderer.cpp:34-46, 54-58`); the transpose it calls a future obligation is implemented
(`PhysicsBridge.cpp:16-22`) and is covered end to end by a real Jolt raycast test that builds a
heightfield, fires at the position the render geometry would draw the peak, and asserts the hit lands
high while the position a naive un-flipped transpose would use is asserted flat
(`tests/landscape/src/LandscapeTreeTest.cpp:279-336`, specifically `:309-331`). `docs/STATUS.md:2623-2637`
carries the current account: stages 1–4 of 5 done, only stage 5 — the editor — not started. **Treat
the README as a lead, not evidence, and fix it separately from this plan**; by its own telling the
renderer and physics bridge don't exist yet, which is the wrong basis for scoping what an editor
needs to build from scratch.

---

## 3. Decisions

| Decision | Consequence |
|---|---|
| Render via a direct call inside `SandboxApp::onRender()`, the same hand-rolled pattern as the existing `objects_` and scene-entity draw loops — not a new `IRenderFeature`, and not routed through `GameRender::drawWorld`/`CMeshRenderer`. | `LandscapeRenderer` has no scene-pipeline hooks and sections are not ECS entities, so forcing either pattern solves a problem this system doesn't have. Visibility in the shipped runtime (`AverGame.exe`) is a second, explicit slice-0 task, not something that falls out of the editor work for free. |
| The `.ocland` quantisation range becomes authored and pinned at section-creation time, not auto-refit from live data on every save. | A `modules/formats` schema change, coordinated with whoever owns its current in-flight work rather than raced against it. Until it lands, exposing "save" to an artist mid-sculpt is exposing the drift bug (blocker 2) live, so this gates any workflow that encourages repeated saves during one sculpt session. |
| Full tree and physics-heightfield rebuilds are debounced to stroke-end (pointer-down..pointer-up), mirroring the existing `beginTransformEdit`/`endTransformEdit` gesture-bracket pattern (`SandboxApp.cpp:2417-2436`). | Mid-stroke feedback needs a separate, cheap preview path that patches already-uploaded geometry directly, bypassing the tree and mesh cache — and that preview must be proven, not assumed, never to silently diverge from the authoritative post-rebuild geometry. |
| A new, landscape-tab-local undo stack, built from scratch rather than extended from `EditCmd`/`undoStack_`. | See §4 — the existing stack is entity-keyed and sized for one fixed transform, with no way to represent a height-sample edit without modelling every touched sample as a fake entity. |
| An undo entry stores sparse before/after height-sample maps, never a whole-section snapshot. | Undo cost and memory are bounded by the stroke's actual footprint, matching the render/physics rebuild's own bounded-region assumption, and the correctness bar becomes directly testable. |
| A new `.ocworld` `LANDSCAPE` record, plus `levelLandscapes_`-style pass-through tracking copying the exact `PCGVOLUME` pattern, lands in slice 0 — before any sculpt or import UI exists. | Closes off blocker 7's silent-revert failure mode from day one, rather than deferring the fix until "the editor can actually change it", which is precisely when the `PCGVOLUME` bug bit in the first place. |
| Landscape collision loads through the same shared `world::instantiate` path both hosts already use (`modules/world/src/LevelInstance.cpp:14-73`), mirroring its existing static-box branch. | Both `Sandbox.exe` and `AverGame.exe` get correct load *and* correct unload for landscape collision from one implementation, not two — the two-copies-of-the-load-loop problem `docs/CHUNKS.md` already fixed once for placements must not be reintroduced here. |
| Texture painting is explicitly deferred past sculpt+save+see-it-in-a-level. | `LandscapeRenderer::draw()`'s single `device.drawMesh(mesh, world, baseColor, metallic, roughness)` call (`LandscapeRenderer.cpp:116`) has no texture/material binding argument at all, so painting is a genuinely new draw path plus a new persisted `.ocland` chunk, not a UI wrapper around sculpting — scoped as its own slice (9) after the sculpt loop is proven. |
| "New landscape" sample-count entry snaps to the exact geometric sequence `build()` accepts — at the default `nodeQuads = 64`, that is 65, 129, 257, 513, 1025, 2049, 4097, not every value of the form `64k+1` (`LandscapeTree.cpp:76-84`; capped at 4097 by `modules/formats/include/aver/formats/OcLand.hpp:18-19`). | A creation action can never produce a section that fails `build()` after the fact with no earlier warning — the constraint is enforced at the UI boundary, not discovered at runtime. |

---

## 4. Undo is a real design problem, not a checkbox

One undo entry is one brush stroke, bracketed at pointer-down and pointer-up. It stores a **sparse
map of `{sample index -> height}` for the state before the stroke, populated lazily the first time
each sample is touched during that stroke and never overwritten again for it**, plus an equally
sparse after-map read once at stroke-end over the same footprint — never a whole-section snapshot.

SandboxApp's only existing undo/redo (`EditCmd`, `undoStack_`/`redoStack_`, `kUndoDepth = 128`,
`SandboxApp.cpp:2374-2385, 5293-5294`) cannot be reused. It is keyed by `EditId <-> scene::Entity`
maps, its payload is `EditXform` — a fixed pair of `Vec3` before/after transforms — and Ctrl+Z/Ctrl+Y
are gated on the Level viewport having keyboard focus (`SandboxApp.cpp:2833-2836`). **There is no way
to represent a height-sample edit as an entity transform without modelling every touched sample as a
fake entity, so the landscape editor needs its own, separate, tab-local stack from scratch.**

The one part of the existing pattern worth copying is the *shape*, not the storage:
`beginTransformEdit`/`endTransformEdit` capture a before-state once at gesture start and compare at
gesture end, skipping the push entirely if nothing actually changed (`SandboxApp.cpp:2417-2436`). A
brush stroke should be exactly one undo entry per pointer-down..pointer-up gesture, never one per
mouse-move sample.

Because a stroke's footprint is variable — unlike `EditCmd`'s fixed six floats — the new stack's cap
should bound total touched-sample count across the stack, not entry count, or a single large stroke
can dwarf 128 small ones and blow straight past the memory budget `kUndoDepth` was sized to avoid.

---

## 5. Slices

Ordering is by dependency: nothing after slice 0 has anything to build against, and nothing after
slice 3 has a safe way to remove stale geometry.

**Slice 0 — prove the render and level-reference path. No editing.**
Link `Aver.Landscape.Renderer` into `sandbox/CMakeLists.txt`; author one synthetic `.ocland` fixture
(none exists on disk today) via a small offline writer; add an owning object — mirroring
`SkinnedScene`'s per-entity residency bookkeeping — that holds one `OcLandData` + `LandscapeTree` +
`LandscapeRenderer` per open section and drives `select()`/`draw()` from `SandboxApp::onRender()`
each frame. In parallel, add the `OcWorldLandscape{name, path, collide}` record (blocker 7) and wire
`world::instantiate` to load it, build collision via the existing static-box branch, and push the
body into `out.bodies` for teardown — the same shared path both hosts already use for placements.

Nothing here requires a brush. What has never been proven by any test in the repo is that the render
and physics paths — individually tested headlessly — survive contact with the real render pipeline
(where the 192-node/1 MiB constant ring contends with live shadow, voxelisation and lit passes) and a
real, shared level-load path. **If that fails, the whole approach needs rethinking before any sculpt
engineering is invested — this is the smallest slice that proves or kills the shape, not merely the
first slice of a longer march.**

**Done when:** opening a level with one `LANDSCAPE` record shows terrain at its authored `originCm`
at interactive framerate with shadow, voxelisation and lit passes all live; closing and reopening the
same level preserves the reference byte-for-byte, with no revert and no loss; a raycast through the
collision body lands within one quantisation step of the rendered height at three sample points; and
`AverGame.exe`, loading the identical level through the same `world::instantiate` call, also shows
and collides with the terrain.

**Slice 1 — editor tab and content-browser recognition.**
Register `makeLandscapeEditor` for `.ocland`; add the fourth `assetIconTile` arm and sprite-sheet
tile; a "New Landscape" action whose sample-count field accepts only the geometric sequence
`build()` actually accepts, never a naive `64k+1`.

**Done when:** double-clicking a `.ocland` file opens a landscape tab showing the section via slice
0's plumbing; fuzzing every `sampleCount` from 2 to 4097 against `build()`'s own acceptance shows the
dialog's accepted set matches it exactly — no value the dialog accepts fails `build()`, and no value
`build()` accepts is refused by the dialog.

**Slice 2 — pin the quantisation range.**
Add an authored, persisted height range to `OcLandData`/the `LHDR` chunk, fixed at section creation
rather than refit from live data on every save; an edit that would exceed the pinned headroom
triggers an explicit "range exceeded — grid is about to coarsen" path instead of proceeding silently.
Coordinated with `modules/formats`' current owner before landing, per the decision table.

**Done when:** the same ten-cycle sculpt-and-save simulation that demonstrated the current drift
(blocker 2), re-run against the pinned-range formula, shows zero perturbation of untouched samples
across all ten cycles while edits stay inside the pinned headroom, and produces the explicit
exceeded-range signal — not a silent one — on a cycle engineered to need it.

**Slice 3 — selective cache and tree invalidation.**
Add `invalidate(nodeIndex)` / `invalidateRegion(sampleRect)` to `LandscapeRenderer`, freeing exactly
the affected resident meshes through the existing `destroyMesh` path, plus a `LandscapeTree` query
returning every node index at every level whose footprint overlaps a given rectangle — neither
exists today. Entirely headless.

**Done when:** a test edits a sub-rectangle, calls `invalidateRegion`, and every node overlapping it
is no longer resident while every non-overlapping node keeps its original handle; the next `draw()`
call re-uploads exactly the invalidated set and nothing else, measured via
`LandscapeRenderStats::created`.

**Slice 4 — the brush: end-of-stroke rebuild with a live preview.**
New ray-vs-heightfield hit testing against the CPU sample grid (blocker 8 — `pick()`/`rayAabb` only
test bounding boxes today); a cheap mid-stroke preview that patches already-uploaded vertex buffers
directly, bypassing the tree and mesh cache; one `LandscapeTree::build()` plus slice 3's
`invalidateRegion` over the stroke's footprint at pointer-up, replacing the preview with the
authoritative rebuild.

**Done when:** dragging across N sampled positions in one stroke produces exactly one tree rebuild,
not N; the live preview tracks the cursor at interactive framerate; the post-stroke geometry from the
real rebuild is bit-identical to an offline CPU rebuild of the same final heights — proving the
preview never silently diverged from the authoritative tree.

**Slice 5 — undo/redo for a stroke.**
The sparse before/after stack from §4, gesture-bracketed, capped by total touched-sample count
rather than entry count.

**Done when:** undo after a stroke restores every touched sample to its pre-stroke value and leaves
every untouched sample bit-identical to before the stroke; redo reproduces the exact post-stroke
state; N strokes pushed then fully undone leave the section bit-identical to before any of them;
pushing past the cap evicts the oldest stroke's patch without corrupting the ones that remain.

**Slice 6 — live collision feedback.**
Debounce physics heightfield updates to stroke-end, exactly like the render tree; via full destroy
(`aver_phys_remove_body`) plus recreate (`aver_phys_add_heightfield`), since no update ABI exists yet
(blocker 6).

**Done when:** a raycast immediately after a stroke ends lands within one quantisation step of the
just-sculpted peak; the pre-stroke collision body is provably gone — no orphaned entry — after every
stroke in a twenty-stroke sequence.

**Slice 7 (deferred — build only if slice 6's measured cost demands it) — incremental physics
heightfield update.**
Add `aver_phys_update_heightfield`, wrapping Jolt's already-present `HeightFieldShape::SetHeights`
instead of destroy-and-recreate.

**Done when:** stroke-to-collision-update latency at a production section size (1025 samples)
measurably drops against slice 6's destroy-and-recreate baseline, under the identical
raycast-correctness assertion slice 6 used. Do not build this speculatively.

**Slice 8 — multi-section budget sharing.**
Either raise the transient constant ring, or enforce a shared per-frame draw budget across all
resident sections, so blocker 9's two ceilings cannot jointly overrun the ring.

**Done when:** a test scene with N sections each near their own 192-node `select()` ceiling in
frustum simultaneously does not exceed the ring's real capacity — measured in submitted draws, not
each section's own `select()` count — and shading is never silently wrong under that load.

**Slice 9 (explicitly deferred) — texture painting.**
A new draw-binding path in `LandscapeRenderer::draw()` (via `IDevice::setDrawBinding`, never called
from this path today) consuming `ChunkMesh`'s currently-dead world-aligned UVs; a new, non-Required
AVR1 chunk in `.ocland` for per-sample layer weights, following the format's own forward-compatibility
precedent for unknown chunks; brush UI for layer selection and blend.

**Done when:** a painted layer persists through a save/reload cycle and renders with a visibly
different material per painted region, in both the editor and — via the same shared `instantiate`
path — the shipped runtime.

---

## 6. What I would not do

1. **Build a brush before slice 0 proves the render-and-level-reference path end to end.** There is
   nothing to sculpt against and no way to see a change until terrain is on screen at all.
2. **Ship `.ocland`'s auto-refit-per-save quantisation into an editor unaddressed.** Any edit that
   moves the section's min or max silently coarsens every untouched sample on the next save; an
   editor that degrades the artist's work every time they save is worse than no editor.
3. **Rebuild the full `LandscapeTree` or the full physics heightfield per mouse-move sample.** A
   1025-sample production section is upward of a million sample visits per rebuild, and Jolt's own
   heightfield build is a further cost on top; always debounce to stroke-end.
4. **Wire brush-stroke undo onto `EditCmd`/`undoStack_`.** It is entity-keyed, Level-viewport-focus-
   gated, and sized for one fixed six-float transform, not a variable-footprint set of touched
   height samples.
5. **Defer the `.ocworld` `LANDSCAPE` record's explicit `levelLandscapes_` tracking until the editor
   can change a reference.** The `PCGVOLUME` incident already proved that gap bites on the first save
   of a level containing the new record, not only once editing UI exists — it lands in slice 0.
6. **Register `LandscapeRenderer` as an `IRenderFeature`, or route it through
   `GameRender::drawWorld`/`CMeshRenderer`.** It has no scene-pipeline hooks, sections are not ECS
   entities, and every existing precedent for this shape of draw call is a hand-rolled inline call in
   `onRender()`.
7. **Add texture painting in the same pass as sculpting.** `draw()`'s `device.drawMesh` call has no
   texture or material binding argument today; it is new rendering and format surface, sequenced last
   deliberately.
8. **Tune the 192-node `select()` draw clamp in isolation from `maxResidentNodes`.** They are two
   independent ceilings hit by different camera behaviour — many sections versus one section up close
   — and tuning only one still silently drops detail via the other.
9. **Accept an arbitrary `sampleCount` in the "new landscape" UI.** Snap to the exact geometric
   sequence `build()` actually accepts, or creation fails after the fact with no earlier warning.
10. **Touch `modules/formats`' `OcLand` schema without coordinating with whoever owns its current
    in-flight work.** Slice 2 depends on it and must be sequenced deliberately, not raced.
11. **Trust `modules/landscape/README.md`'s stage status over `docs/STATUS.md` or the code itself.**
    The README is stale, and this session's own reads of `LandscapeRenderer.cpp` and
    `PhysicsBridge.cpp` already contradict it.
