# Changelog

Every entry here is a public release — a tag on
[Vectoric-Core-Systems/Aver-Engine-download](https://github.com/Vectoric-Core-Systems/Aver-Engine-download/releases).
Format loosely follows [Keep a Changelog](https://keepachangelog.com/); versioning is
`MAJOR.MINOR.PATCH` while the engine is pre-1.0 and every release is a beta. Commit hashes in
parentheses are short refs into this repository — `git show <hash>` for the full reasoning, which is
usually longer and more useful than the line here. `docs/BUGS.md` carries the full defect list with
triggering inputs for anything summarised below.

## [0.5.0] — 2026-09-08

The release where the default renderer started telling the truth. Aver's default path is ray-driven
primary visibility — rays, not a rasteriser, decide what every pixel sees — and until now it shaded
each of those hits with one flat colour per material. Textures existed; the renderer that actually
paints the screen could not read them. That is fixed, and fixing it turned out to make the frame
*faster* rather than slower. Around it: 259 commits of the same shape, where the interesting part is
usually how long something had been quietly not working.

### Rendering

- **The default renderer samples real textures.** Ray-driven primary visibility shaded every hit
  with a per-material average colour; a brick wall and a painted wall were the same wall. Against a
  raster reference on ElectricDreams, mean absolute difference fell from **16.76 to 7.48** out of 255
  — that is the settled figure after the world-aligned-UV fix landed the same day and brought the
  remaining seven texture slots into the comparison. Reflections and glass panes sample textures too,
  through the same table.
- **And it got faster.** Mip selection moved to ray differentials, which is both the correct answer
  and a cheaper one: whole-frame median **11.96 ms → 8.14 ms** on ElectricDreams (−32%), 15.47 ms →
  14.21 ms on PTTest. Texturing's own cost went from 4.1 ms to **0.3 ms** over the untextured path.
- **Multi-material meshes stopped rendering in one colour.** Anything imported from OBJ, glTF or USD
  with more than one material slot — trees, characters — drew wearing whichever slot happened to win.
  Separately, a single-submesh mesh with an authored material and no per-placement override drew flat
  grey: every plant in the demo content. Probe (0.53, 0.80) went from grey **(186,180,181)** to
  foliage green **(20,30,14)**.
- **Alpha-masked geometry casts the shadow of its cutout.** Foliage, grates and chain-link were opaque
  to every ray, so a leaf card threw a solid rectangle. Judge it by the shadow, not the silhouette:
  240 plants cost **+0.47 ms** GPU (+8%) and the light on the ground goes from a filled blob to
  leaf-shaped dapple.
- **Ray-traced shadows stopped flickering under camera motion.** The temporal branch used at every
  quality tier never read its own history back. Motion deviation in the 9–32 code band fell
  **2.445% → 0.843%**, and 33+ codes **0.177% → 0.078%**. The still image also got closer to an
  eight-ray reference.
- **Ambient occlusion accumulates over frames**, and runs at High rather than only Epic. It used to
  fake resolution by sharing one ray across a 4×4 tile, which reads as blockiness rather than noise.
  One ray now replaces four: the ray-driven primary pass went **19.1 ms → 16.72 ms** at essentially
  unchanged darkness (mean luminance 20.71 → 20.67).
  - **The history buffer it was added for had been accumulating nothing**, because the AO ray traced
    the *identical* direction every frame. It rotates by the golden angle per frame now. Dark-region
    local roughness **0.4508 → 0.3838** at unchanged mean luminance.
- **Tone mapping stopped desaturating.** The old fit pulled saturated colour toward grey; a matrixed
  Hill ACES fit gives **+13% mean saturation** at essentially equal brightness.
- **Water and glass tint what is behind them.** Absorption is per-channel over a ray-measured path
  length instead of a flat surface colour or a colourless darkness scalar. Glass edge-on greenness
  **+0.118** against +0.002 for a low-iron control; the pool floor's shadow went (19,33,42) →
  (33,40,49); magenta pixels across three test cameras went from tens of thousands to **0**.
- **Water is a material, not a renderer.** It drew through a bespoke pass bolted on beside the
  pipeline, which is why it never cast a ray-traced shadow, never reached GI, and was invisible to
  the acceleration structure. It is an ordinary translucent mesh now. It is also not free: at the
  overhead pool camera the frame went **28.8 ms → 40.4 ms**, of which the water itself is 11.6 ms.
  A scene with no water is unchanged (8.107 ms against an 8.033/8.063 control).
- **Refraction is a real feature** — screen-space and ray-traced, tier-derived with strength and edge
  fade left reachable. Off versus ScreenSpace differs on 31.00% of pixels, Off versus RayTraced on
  31.51%.
- **Unlit view mode goes unlit.** It rendered pure white, and only ever addressed the raster path —
  not the renderer that actually draws the scene. Sponza's centre probe went **(10,10,12) →
  (173,171,167)**, matching raster. Where a view mode structurally cannot work it now disables itself
  with a stated reason instead of silently doing nothing.
- **A material graph can animate, and can author a volume.** A `Time` node — wrapped hourly so float
  precision holds — reaches a material shader for the first time, so ripple, scroll and flicker are
  possible at all; `AttenuationColor` and `AttenuationDistance` make the absorption above authorable
  from the graph rather than only from a `.ocmat`.
- **Caustics are projected from the water volume**, computed analytically from the ripple's own
  curvature and applied into the sun term, so they correctly vanish inside a shadow. Clipped to a
  true per-mesh AABB — `createMesh` had been measuring one and throwing it away.
- **A project asking for `VOXELRES 512` gets 512.** The GI volume is sized once in
  `VoxiRenderer::init()`, and the manifest's value was applied one step too late — after the volume
  had been built — so a project requesting 512³ silently ran at 128³, 64× smaller, and was told it
  had been applied.
- **The path tracer became a reference worth measuring against.** It was a Lambertian approximation
  with one flat colour per mesh instance; it is a full energy-conserving PBR integrator now. Furnace
  test, white conductor at roughness 1: **0.307 L → 0.9976 L** after multi-scatter compensation. It
  samples base-colour textures too.

### Performance

Every figure here is from the commit that made the change, on this machine, with vsync off.

- **A path-tracer quality tier was rendering the whole scene twice, every frame, converging an image
  nobody ever saw.** Default PTTest frame: **14.765 ms → 7.955 ms** — 6.8 ms, 46% of the frame.
- **The cascade shadow atlas was cleared and drawn for nobody.** With ray-traced shadows on, nothing
  ever sampled it. Frame median **47.543 ms → 40.112 ms** (−15.6%), p90 48.057 → 40.658, and the
  output is bit-identical (MAD 0.0000 across 929,500 pixels).
- **Scene load stopped building a second acceleration structure per mesh.** Voxi and the path tracer
  each built their own BLAS for the same geometry. On Sponza, `createBlas` at load went
  **154.3 ms (220 built, 0 reused) → 0.1 ms (0 built, 220 reused)**, and resident BLAS memory halved.
- **Shader compilation dropped from 5.6 seconds a launch to 21 milliseconds warm.** A disk-backed
  DXIL cache keyed on the whole compiler input: before 64 compiles / 5,601 ms; cold 49 / 3,188 ms;
  warm **0 compiles / 20 ms**, 64 of 64 served from 1.2 MB on disk, bit-identical DXIL. D3D12 only —
  Vulkan still pays it.
- **The most expensive single operation in the renderer under motion was a GPU readback for a GI
  volume that could never be cached anyway.** GPU total **51.11 ms → 36.25 ms** (−29%); the
  `Voxi GI update` exclusive scope went **10.79 ms → 0.00 ms**.
- **GI scales to a frame-time budget** (`RENDER.FRAMEBUDGETMS` / `--frame-budget`) instead of running
  authored quality regardless of scene cost, on a rung ladder of update interval then cone count. At
  `--frame-budget 33` over 250 frames a 68.6 ms average steps through rungs 1→4: GI update
  **21.83 ms → 9.20 ms**, voxelise **10.54 ms → 1.67 ms**.
- **The GI derived-data cache stopped rebuilding itself fifteen times a minute** because a scene had
  clouds in it. A 900-frame run under camera motion wrote 439 MB across 24 entries over 17 distinct
  keys and swept all 24 away again; a run 4.5× longer now writes **18.7 MB across one entry** and
  sweeps nothing — about 100× less cache writing. Separately, nudging the sun while lighting a level
  no longer costs an 18 MB write per nudge.
- **`RENDER.VOXELRES 512` stopped stalling on disk on every camera move.** Wall clock
  **17.7 s → 5.6 s**; bytes written **9.4 GB → 0**. GPU per frame 12.8 → 13.4 ms: the GPU was never
  the problem.
- **Toggling ray tracing at runtime stopped leaking about a third of a gigabyte per cycle**, and Low
  and Medium tiers stopped allocating a **112 MB** ray-traced ambient buffer they never read.

### Aver Node

- **The palette is searchable.** Type to filter across **240 node types in 23 categories**, ranked
  name-starts-with, then contains, then category or type id — instead of hunting a right-click
  submenu tree 23 entries wide.
- **Drag a wire into empty canvas and it builds the node it needs**, palette pre-filtered to what can
  accept that pin, wired up on pick. The standard gesture, previously absent.
- **Right-click on a node does something.** Cut, copy, paste, duplicate, delete and Break Links,
  rather than always opening Add Node. Ctrl+C/V/D work on the canvas, paste lands under the cursor,
  and only links with both ends copied come along.
- **A Const node's value can be typed on the canvas** instead of only in a hand-edited `.ocgraph`.
- **A graph can name its own entity**, via a new `Self` node — a graph built entirely on the canvas
  previously had no way to refer to the thing it was attached to.
- **The editor can ask whether a graph is valid.** A Validate button calls the real C#
  `Graph.Validate()` and rings the offending node in amber. An exec output wired twice now fails by
  name; it used to surface only as an engine-log exception the first time the finished graph ran.
- **Visual scripting has a debugging surface at all.** A `PrintString` node that fires on exec, an
  on-screen feed in the viewport corner that collapses consecutive duplicates into one row with a
  count, and — separately — the canvas rings a node green for 1.5 s when it *actually executes*.
  Those are recorded hits from the compiled IL, not an animation, so you can watch which arm of a
  Branch, Switch, Gate or ForEach really ran.
- **27 physics nodes** reach forces, impulses, body material properties, motion state, collision
  layers and five joint types with motor control. **Three input-action nodes** (`InputAction`,
  `InputActionPressed`, `InputActionReleased`) reach the named, priority-stacked action layer.
- **A graph-only game — zero C# files — can be packaged.** Before this it could not be packaged at
  all, on any machine, for any project. A staged package declared three graph classes from its own
  Content with no C# and no compiled scripts anywhere in it.
- **Browsing a level stopped running its gameplay graphs.** Over 1000 frames with Play never pressed,
  a GameMode graph's own `elapsed` variable climbed to 12.31 seconds across 4003 tick lines. After
  the fix: 300 frames of Select produce **0 OnTick and 0 begin_play**, while 200 frames of Play still
  produce both.

### Editor

- **The loading screen covers the whole load, on every path a project opens from** — and the editor
  no longer opens a console window. It is linked `/SUBSYSTEM:WINDOWS` now, so Windows never creates
  one; run it from a terminal and it attaches to that terminal's console instead, so scripts and
  tooling that read its output are unaffected.
- **Undo stopped lying about selections.** Undoing a multi-object move returns the whole set, not
  just the object you grabbed. Duplicate and Copy/Paste carry children. Deleting a parent takes — and
  undo restores — the entire subtree. Undoing an edit on a child no longer teleports or orphans it.
  The ~25-slider material panel is on the undo stack at all.
- **Multi-select works in the Outliner and the viewport**, every selected object gets an outline, and
  F, Delete, Copy, Paste and Undo work when focus is in the Outliner or Details panel rather than
  only the 3D viewport.
- **The World Outliner is a tree you can drag objects around in** to reparent them, with a search
  filter.
- **Settings actually reach disk**, however you change them and however the editor closes. Project
  Settings gained a Save/Revert footer on every page, editable project name/author/start map, live
  Physics and Audio pages, and four render settings the manifest could not hold. Post-process
  settings persist. Sun and fog edits from the Details panel survive a save.
- **Autosave warns before it saves and lets you postpone**, and runs every ten minutes rather than
  every thirty seconds. A level that has never been saved is autosaved too, and Details-panel edits
  finally count as unsaved changes — as does the "Unsaved changes" exit prompt, which previously did
  not mean it.
- **A real notification system** — a bottom-right toast stack — replaced a status-bar string that
  decided its own colour by guessing at English. Long operations (C# compile, navmesh bake, GI cache
  flush) report progress and completion; every import reports its outcome, including two paths that
  previously reported nothing at all.
- **Deleting or renaming an asset tells you what it will break, first.** Renaming can repoint
  everything that referenced it, opt-in. "Find References" is a standalone tool rather than only a
  side effect of deleting. Content Browser search reaches subfolders, supports Ctrl/Shift/Ctrl+A
  multi-select, and a drag carries the whole selection; dropping onto a folder asks Copy / Move /
  Cancel and warns what a move will break.
- **A dropped asset rests on the surface it landed on** instead of burying itself inside it. Objects
  can be grabbed directly rather than only by hitting a thin gizmo axis line. Scrolling with
  right-click held speeds the camera **up** — that control was entirely dead code, and then briefly
  backwards.
- **Keybinds got their first automated tests**, the bindable key list grew from **53 to 90**, and
  rebinding on the Preferences page rebinds everywhere including the Graph Editor.
- **Twelve editor controls that looked like they worked, didn't** — seven silently doing nothing,
  five working internally but unreachable from the UI. Found by sweeping the interface rather than
  reading the code.

### Play in Editor

Pressing Play was broken in basic ways, and each of these was a separate defect.

- **Play starts at the Player Start, facing the way it points.** You could place one, drag it, and
  save it into a level as a spawn record; Play ignored it — for the default pawn *and* for a
  project's own GameMode pawn. Before that landed, Play spawned at world (0,0,0), which in an empty
  test level is open air and in a real level is underground: on ElectricDreams it put the camera
  inside the ground.
- **Play with no GameMode gives you a camera**, not a stationary drone you are stuck inside. The
  drone only moved if the project had separately named a flight graph through a CLI-only flag, so
  for anyone using the editor normally, Play snapped the view into a parked quadcopter.
- **Mouse look works.** It was not glitchy, it was dead: the fix for the pawn's movement had put the
  look block inside a `!gameHasInput()` guard, and the default pawn counts as live input, so the
  block excluded itself. `--pie-camera-test` is what caught it.
- **Stop puts the level back where Play found it.** Play was a one-way door — every body physics had
  shoved, every actor a graph had moved, stayed moved, and the only route back to the authored level
  was reloading and losing unsaved edits.
- **Releasing the mouse mid-session no longer leaves every key held down forever.**
  `aver_fw_input_new_frame()` rolls `cur` into `prev` but does not clear `cur[]`, so an early return
  in `pushInput()` meant "republish last frame forever" rather than "publish nothing". Hold fire, hit
  the release-mouse chord to click something in the Outliner, and the weapon kept firing.
- **Clicking back into the viewport hands control to the game, and does not fire the weapon doing
  it.** The gesture never worked at all: ImGui always reports `WantCaptureMouse` over its own dock
  window, so the guard could never be satisfied.
- **File > Open Level opens a level.** The menu item was not a picker — it called `loadStartMap()`,
  which only ever reopens the project's single start map, so a project with more than one level had
  no menu path to any of the others.
- **The selection outline exists in the default renderer.** It was gated on `!sceneSuppressed()`, and
  ray-driven rendering — the standing default — suppresses the rasteriser entirely, so the outline
  was drawn into nothing for the render mode everyone actually uses.
- **The gizmo draws where the mesh is.** Selecting a child under a parent at, say, (5000,0,0) put the
  move/rotate gizmo 50 m away from the mesh, and dragging it moved the child against the wrong
  origin.

### Physics, animation and UI

- **The physics ABI went from 49 functions to 125, all of them bound in C#** — forces, impulses,
  torque, live mass/friction/restitution/damping, sleeping control, per-body gravity. None of that
  existed before.
- **All twelve Jolt constraint classes are usable** — fixed, point, distance, hinge, slider, cone,
  swing-twist, six-DOF, gear, rack-and-pinion, pulley and path — with motors and runtime limits.
- **Collision layers do something, and filtered raycasts exist.** The character controller can climb
  kerbs, crouch, and report what it is standing on: the same character on the same 25 cm kerb travels
  **746.12 cm** with a 40 cm step-up and stops dead at **100.00 cm** without one.
- **Physics can be authored in a level** — `CRigidBody` and `CJoint` components, serialized and
  editable in the Details panel like any other.
- **Two-bone IK** is the engine's first inverse-kinematics solver, on a new pose-modifier seam
  between sampling and skinning, with a `.ocrig` text format and a `CControlRig` component. The wrist
  lands on the goal to **0.00 cm**; with no rig it sits 128 cm away.
- **The game UI draws text and detects clicks.** `modules/ui` previously exposed `addRect` and
  `addTexturedRect` and nothing else — no font, no glyph, no hit test anywhere in the module.
- **Audio buses are type-safe end to end**, an `Aver.Framework.Bus` enum checked against the native
  header rather than a bare `int` a script could point at the wrong bus.

### Importing

- **One shared material shape backs glTF, OBJ and USD**, and USD materials are read for the first
  time via `UsdPreviewSurface`/`UsdUVTexture`. glTF import writes real `.ocmat` files and copies the
  textures they reference instead of dropping everything past the geometry, and the shipping asset
  compiler does that cook — not only the developer test harness.
- **Multi-part scene imports place correctly.** Every piece's pivot was scattered away from its own
  mesh: on Intel Sponza, **115 of 115 mesh nodes displaced, median 10.74 m, max 20.69 m**. The
  importer now also writes a `.ocworld` recording where each piece goes, from the editor's own Import
  button as well as the command line.
- **Foliage stopped importing wrong.** USD trees came in as one solid-wood mesh with the wrong
  colours and opaque cards. Three separate opacity/albedo bugs are fixed, and the LOD builder stopped
  storing ~30 near-duplicate copies of the same mesh: across Intel's Jungle Ruins set,
  **1.44 GB → 271 MB**, with `JR_riverforest` alone **845 MB → 101.7 MB and 45 s → 10 s**.
- **`--max-texture` caps imported texture resolution** with colour-space-correct downscaling — twelve
  textures went **1024 MB → 256 MB** of VRAM. Opt-in; an import with no flag is unchanged.
- **Levels can express a parent/child hierarchy**, and the latent bug that would have corrupted every
  child's transform the moment one existed is fixed in the same change. The level environment format
  grew from 5 saved fields to about 30, so sun colour temperature and disk size, the sky dome, six
  air parameters, ground and sky light, height fog and all seven Volumetric Clouds controls persist.
- **A `.ocproject` or `.ocgraph` from a newer engine is refused** rather than silently read with
  unfamiliar fields dropped and then rewritten that way.

### Backends and shipping

- **Vulkan is on by default and renders the full editor** — docking, viewport, outliner, details —
  after two register-macro text-matching bugs and making unsupported mesh pipelines fail safely
  instead of aborting the driver. Reported in the status bar as *Vulkan | AMD Radeon RX 7800 XT |
  62 FPS (16.18 ms)*. Vendored headers only, no SDK dependency. **It is not at parity with D3D12** —
  see Known issues.
- **`AverGame.exe` packaging is back, with a check that can fail it.** `verify-game.ps1` takes a
  scene census on both the editor and the packaged game — entity counts plus an order-independent
  hash of every (mesh, material) pair — and fails if they diverge. It is a census and not a frame
  diff on purpose: the two hosts have different aspect ratios and cameras by design. It found a real
  defect on its first run, a package shipping the HLSL compiler and zero HLSL files.
- **Packaging can compile a project's C# headlessly** (`stage-game.ps1 -Compile`), through the exact
  build path the editor's Tools menu uses, so a pipeline can produce a fully built package with
  nobody clicking a button.
- **A shipped game can show its own crash** (the crash reporter ships with it) **and read its own GPU
  timings** (`--stats [seconds]`), through one shared formatter, so the editor console and the
  shipped game can never print different numbers from the same data.
- **One vocabulary for exit codes** (0 ok, 1 failed, 2 usage, 3 environment, 4 interrupted) and for
  ABI results, replacing conventions that conflated a failure *count* with a process exit code. Crash
  kinds are frozen: out-of-memory reports as `OutOfMemory` rather than a generic crash.
- **Seven native/managed ABI boundaries that agreed only by convention now agree by construction** —
  most seriously two script bindings that were silently corrupting non-ASCII strings in both
  directions.
- **New Project worked.** The editor's own creation flow, `--new-project` and every template scaffold
  wrote an `.ocproject` the loader immediately refused, then deleted the half-made folder.

### Breaking changes

- **`AVER_RHI_VULKAN` defaults to ON.** A default build now compiles and links the Vulkan backend
  alongside D3D12/D3D11.
- **Windowed is the default again**; borderless fullscreen moved behind `--fullscreen`. Capture runs
  were always windowed, so no recorded probe moves.
- **A `WATER` record must name a `material <name>`** (e.g. `M_Water`) or the volume draws nothing —
  water no longer has a renderer of its own to fall back on.
- **`fluids::FluidMaterial` is now `FluidPhysicsMaterial`** (~70 call sites). It only ever carried
  particle mass and Jolt damping, never an appearance; a project naming a fluid preset for its look
  was never doing what it looked like it did. **`AVER_WATER_MAX_WAVES` is now `AVER_MAX_WAVES`.**
- **`MaterialConstants` grew 112 → 144 → 160 bytes** (texture-slot indices, then `attenuationColor`
  and `attenuationDistance`), with matching HLSL cbuffer and `RtMaterial` changes.
- **Framework ABI minor 4 → 5** (named actions, raw VK input, gamepad shape). The Scene ABI gained
  `aver_scene_last_error()` at minor 5.
- **A future-versioned `.ocproject` or `.ocgraph` now fails to open** instead of loading, dropping
  fields it does not understand, and rewriting the file that way.

### Known issues

- **Vulkan is not at parity with D3D12.** Ray hits cannot be textured — the bindless texture table is
  an explicit stub that warns and returns 0 — so Vulkan gets the flat-albedo ray path this release
  spent its time replacing. Mesh-shader pipelines that read mesh geometry are refused rather than
  built, because AMD's compiler aborts the process outright on the descriptor mismatch; Voxi falls
  back to its slower geometry-shader path. The cascade shadow map is still never written. There is no
  shader blob cache, so Vulkan pays the full compile every launch. **Use D3D12 for anything you care
  about looking right.**
- **A mesh still renders with one material.** Multi-material imports now name and load every slot
  correctly, and the ray path shades them — but `CMeshRenderer` carries a single material for the
  whole renderer and nothing in the raster path binds per-submesh.
- **GI still rebuilds on roughly 70% of ticks under camera motion.** The cause is now named rather
  than guessed at: a culled entity's handle swaps from N per-part draw handles to one base handle at
  the frustum boundary — same entities, different handle shape. Four earlier hypotheses were refuted
  by measurement. The candidate fix is estimated at **+64 ms** on ElectricDreams and was deliberately
  not shipped pending that being measured properly.
- **Frame determinism regressed with temporal accumulation.** A still-camera capture compared against
  another still-camera capture used to be bit-identical; about **2.3%** of pixels now differ run to
  run. Gate probes sit in flat neighbourhoods and stayed stable across 25 runs, so the render oracle
  is unaffected — but a whole-frame diff is no longer a valid test.
- **The render gate baselines are stale, and this build ships with them red.** All 20 Release gates
  fail, every probe uniformly darker than a baseline recorded before the tone-mapping fix and the
  ambient work above. The verifier that decides whether a staged payload matches the tree it came
  from does not depend on the baseline and passed; the baselines themselves are re-recorded
  deliberately, not as a side effect of a release.
- **Screen-space refraction can only bend what the camera already sees**, so an offset can land
  off-screen or on something in front of the glass. Edge fade softens it; the limitation is
  structural.
- **Rotate and Scale gizmos still act on the anchor object alone**, not the whole multi-selection —
  choosing the pivot for a group rotate or scale was left open rather than decided silently. Sibling
  reorder and multi-select drag in the Outliner are not built.
- **Persistence and Synapse received no functional change this release** beyond one `restore()`
  teardown fix. The `.ocsave` format and its I/O layer are otherwise untouched.

## [0.4.0] — 2026-08-25

The release where the things that were already built get wired to something. A complete audio stack
had zero callers anywhere in the tree. Nothing outside a test could save a game. The Vulkan backend
was a stub that returned `nullptr`. Each of those is now reachable from a graph or from a running
editor. Alongside that, the one genuinely new capability: a material can be authored by wiring nodes
instead of writing C#.

### Materials

- **A material can be a node graph.** A `.ocgraph` that declares `DOMAIN material` compiles to HLSL
  and shades a real surface; a `.ocmat` binds one with a `GRAPHREF` line. 56 material node types —
  constants and inputs, texture sampling against the eight existing slots, generic maths that widens
  by arity, vector operations, UV transforms, procedural noise and checker, Fresnel, BlendNormals.
  Previously a surface was a C# class and there was no other way to make one; `.ocmat` parsed a
  `GRAPH{}` block only to skip it with a warning that no material compiler consumed it.
- **Every graph shares one shader.** The generated `averEvalMaterial` is dispatched per draw by an id
  in the material constants, so a project with two hundred graph materials compiles the same three
  shaders as a project with one. The naive shape — one pipeline per material, multiplied by each of
  the three shaders that call into material evaluation — was rejected for that reason.
- **A graph may drive as little as it likes.** What a material *authors* was split from what the BRDF
  *derives*, so a graph that sets only base colour leaves metallic, roughness and the rest on the
  existing constants rather than zeroing them.
- **The C# route is untouched and is not going away.** A `.ocmat` with no `GRAPHREF` takes exactly
  the path it took in 0.3.0, through code the graph work never modified. That was the sequencing
  constraint the whole feature was built under: no stage of it could leave two half-finished material
  systems in the tree.
- **A `.ocgraph` says which language it is written in.** A `DOMAIN` record directly under the header.
  Absent means gameplay, so every graph written before this release keeps working untouched; an
  *unrecognised* domain is deliberately treated as "not mine" and skipped rather than guessed at.
  Without it, gameplay class discovery swept up material graphs and reported them as broken actor
  classes.

### Aver Node

- **The node catalog went from 40 entries to 209.** Of those, 153 are gameplay nodes, up from 40 at
  0.3.0. The gap being closed is a specific one: most of the C# gameplay API had no node at all, so
  anything past the handful of shipped verbs meant dropping out of the graph and writing a class.
  New this release, among others: full vector maths (`VecAdd`, `VecCross`, `VecNormalize`,
  `VecLerp`, `VecDistance` and the rest); character state (`GetVelocity`, `SetVelocity`,
  `IsGrounded`, `Teleport`); session and possession (`GetPlayerPawn`, `GetPlayerController`,
  `GetGameMode`, `IsPlaying`, `Possess`, `Unpossess`); plus the save, audio, animation and Synapse
  families described elsewhere in these notes.
- **`InputKeyPressed` and `InputKeyReleased`** report the rising and falling **edge** of a key. This
  closes a known issue from 0.3.0: firing was level-triggered, so holding the key fired every tick
  unless a graph built its own cooldown out of a `DoOnce` and a variable per key. The framework ABI
  had answered the question directly all along and no node reached it. The output pin is named
  `triggered` rather than `down` because it is an event, not a state.

### Fluids

- **`Aver.Water` and `Aver.Render.Fluid` are one module, `Aver.Fluids`.**
- **A fluid is authored in units you can look up** — density in kg/m³, dynamic viscosity in Pa·s —
  with the raw XPBD compliance and damping knobs still reachable underneath. The two are not the same
  kind of number and the docs say so: density is genuinely physical, with particle mass derived as
  ρ·V/N, while viscosity is a fit calibrated against a measured damping-to-decay sweep.
- **A level can author its own bounded water with `WATER … SIMULATED`**, which runs through the solver
  instead of sitting still. The FirstPerson template's pool sloshes rather than looking like painted
  plastic.
- **`COMP <id> Fluid`** places a fluid from a graph, and the graph editor previews it. The preview
  draws the static seed shell computed from the component's description — it deliberately does **not**
  simulate, because the physics step is gated on Play and stepping the one shared physics world for a
  preview would falsify an invariant the tests pin.

### Synapse — seeing and deciding

- **An NPC can see the player and react.** `CSynapsePerception` gives an agent a sight cone and an
  `OnSeeTarget` event, `CSynapseAgent` gives it a tick and steering, and a `.ocbt` behaviour tree
  decides what it does about it. The tree has its own format, evaluator, scene join and editor tab.
- **Basic baked-navigation wall avoidance**, so an agent walks around geometry rather than through it.

### Sound

- **The mixer has callers.** Six Aver Node nodes — `PlaySound`, `PlaySoundAt`, `StopSound`,
  `IsSoundPlaying`, `SetListener`, `SetBusVolume` — connect `Aver.Audio` to gameplay. The mixer, the
  WASAPI device, the 24-export C seam and its 72 headless assertions all shipped in an earlier release
  and **nothing in the entire tree had ever called any of it**.
- **Aver Sound**: a procedural sound effect is authored as a node graph and rendered **offline** to
  PCM, with its own `.ocsnd` editor tab. Offline is a consequence, not a shortcut — the mixer's `mix()`
  forbids allocation and locks, so a graph cannot be evaluated on the audio thread. Real-time
  evaluation is designed for and not built.

### Saving

- **`SaveGame` and `LoadGame` are exec nodes.** A world is captured and restored without the engine
  needing to know what any given component is. Before this, nothing outside a test could save
  anything at all — a shipped game had nowhere to remember anything.
- **A restored actor's `OnBeginPlay` runs against its restored state**, not against class defaults.
- **Graph variables persist across a save** for GameInstance-scoped actors.

### Animation

- **A rig can name attach points.** A socket editor places them per-rig, and a graph attaches an
  object to a hand socket with no C# involved — a first-person weapon now attaches by wiring alone.
  Previously there was nowhere on a rig to say where the weapon goes.
- **Clips carry notify events** with a name and a float payload that reaches C#, so a footstep or a
  reload click fires on an exact frame.
- **The FirstPerson template character has a body.** It was previously disembodied.
- **A glTF node's transform reaches the skeleton** however the source file spells its joint names.
  Baked-matrix joints had been collapsing to identity.

### Vulkan

- **The backend presents frames.** At 0.3.0, `modules/rhi.vulkan` was 8,774 lines that had never
  once been compiled — a backend written on paper. Twenty-seven commits since have made it build,
  construct a real device, present, run the editor's own UI, and do instanced draws, at 11,802 lines.
  **It remains off by default** (`AVER_RHI_VULKAN=OFF`), so a default build is byte-for-byte
  unaffected; what changed is that a developer who switches it on now gets something that works
  rather than a silent null device. One known defect is listed below.
  (`docs/VULKAN.md` counts the same work from its 2026-08-09 checkpoint instead, so its commit
  figure is larger; this one is measured from the 0.3.0 cut, `c099159`.)
- **The LunarG SDK is no longer a dependency**, replaced by vendored headers and a vendored
  SPIR-V-capable DXC.

### Editor

- **Double-clicking a `.ocmap` or `.ocworld` opens it.** Neither extension was registered, so a
  double-click reached Windows' generic "how do you want to open this file" chooser. A second
  double-click now finds the editor that is already running and loads the level into it rather than
  starting a second process.
- **The Content Browser shows real thumbnails**, rendered rather than placeheld, with a per-type icon
  and colour identity.

### Rendering and stability

- **Fixed: raising path-tracing quality on a running view could remove the GPU.** Resizing the
  accumulator left the denoiser's descriptors describing the *new* size over a buffer allocated at the
  *old* one; the shader walked off the end and the device was lost with
  `DXGI_ERROR_DEVICE_HUNG`. It read as intermittent and setting-specific because whether an overrun
  faults depends on what happens to be allocated past the end. Views are now rebuilt on every re-arm,
  and separately **the RHI now refuses an oversized view outright** — D3D12 accepts one at creation
  without the debug layer, so nothing between the mistake and the dead GPU had said a word.
- **A lost device stops the frame instead of crashing the process.** A removed or hung GPU is detected
  once, logged with its reason decoded, and named in the window title; the frame loop then stops
  drawing. Previously both callers of the detection discarded its result, so a dead device was handed
  a fresh command list every frame, each paying a full one-second fence timeout, and the process
  eventually died with no message. It does not try to recreate the device — that means recreating
  every resource every module owns — so it freezes on the last good frame, deliberately, to leave the
  explanation on screen.
- **The ray-tracing geometry table is built per mesh, not per instance.** A scene with many copies of
  one mesh was paying gigabytes and tens of milliseconds rebuilding it every animation frame.
- **Baked GI can be cached to disk** beside the project and read back on the next run.

### Known issues

**Rendering**

- **Vulkan never writes the shadow cascade map.** `pushRenderScope` discards the caller's render
  targets, so the cascade pass renders into nothing. Ray-traced shadows mask it, which is why it was
  not obvious. Vulkan is off by default, so a default build is unaffected.
- **Global illumination can still be dominated by a large, saturated, brightly-lit surface.** The
  gather is clamped per component (`AVER_VOX_MAXRAD`, 16.0), which bounds the magnitude but not the
  hue: a big pure-red emitter clamps red at the ceiling while green and blue stay low, and the frame
  goes red. Three 1.8-metre red spheres under a 100,000-lux sun reproduce it. Scaling them down is
  the workaround. *(A comment in `Voxi.hpp` still says the gather is not clamped at all. That is out
  of date — the clamp exists.)*
- **One render gate is green but blind.** `rt-penumbra` exists to sample a partially-occluded
  ray-traced pixel, and the pixel it samples is now fully shadowed, so it no longer measures
  ray-traced soft-shadow quality. It needs its probe re-picked, not just re-recorded.

**Fluids**

- **A fluid volume collapses.** It simulates, but it pancakes — even at the proportions its own unit
  calibration produces.
- **The fluid preview in the graph editor does not move.** It draws the static seed shell computed
  from the component's description. This is deliberate: the physics step is gated on Play, and
  stepping the one shared physics world to animate a preview would falsify an invariant the tests
  pin. It is a placement and sizing aid, not a simulation.

**Audio**

- **Aver Sound renders offline only.** A `.ocsnd` graph is evaluated ahead of time into PCM. It
  cannot be evaluated live, because the mixer's `mix()` forbids allocation and locks and a graph
  needs both. Real-time evaluation is designed for and not built.

**Materials**

- **A material graph is resolved by the editor and nothing else.** `GRAPHREF` resolution lives in the
  sandbox; no engine module reads it. A graph material works when you run your project through the
  editor, which is currently how a project runs at all — see below.
- **No vertex or displacement graphs, and no per-graph exposed parameters.** A graph drives the
  surface, not the geometry, and its constants are baked at compile rather than surfaced as tweakable
  material parameters.
- **Vulkan parity is unverified.** The generated HLSL has only been exercised on D3D12.

**Projects and the editor**

- **A project scaffolded from the 0.3 FirstPerson template still renders the inside of its own
  character.** The fix — ` hidden=owner` on the body's `COMP` line — was made in the template, and
  upgrading a project deliberately does not go and edit graphs you own. Open your character graph,
  find the `COMP` line for the body mesh, and add ` hidden=owner` to it. A project created fresh in
  0.4.0 already has it.
- **The packaged-game path is back, with a divergence gate.** `AverGame.exe` was deliberately removed
  in 0.3.0 because a second host could render a different subset of the scene and nothing could
  notice. It is restored, and `scripts/verify-game.ps1` now compares both hosts' scene censuses
  (entities, meshes, materials, and a hash of every mesh/material pair) so that divergence fails
  instead of going unseen. The gate found a real defect immediately: `game.allowlist` shipped the
  HLSL compiler and no HLSL, so a package fell back to the Null backend and drew nothing.
- **No 2D HUD from a graph.** Nothing in the node vocabulary draws text or 2D, so a game's score can
  still only be read from the Output Log. Unchanged from 0.3.0.
- **Saving a level reloads it.** The editor watches its own files, so a save triggers a reload that
  discards undo history, selection and camera position.
- **The animation editor reloads over unsaved edits.** Same watcher, same consequence, and here it
  can cost work.

**Legacy `.ocmap` levels**

- A `DEFORM` placement is shown in the editor as a small static box. There is no deformable-cage
  system to render it properly; it round-trips faithfully on save regardless.
- A `PLACE` with a non-uniform scale collapses to its X scale on save, because the legacy record has
  one uniform scale field. No editor UI can author a non-uniform scale on one today, so this is not
  currently reachable.

**Capability fallbacks**

- **Pre-SM6 hardware, or `--force-caps no-dxc`, silently drops several modules** — UI, skinning,
  fluids, particles and occlusion culling — without saying so.
- **The occlusion culler retries its failed shader compile every frame** rather than giving up once.
- **The Voxi voxel-grid resolution control does nothing after startup.**

**Documentation**

- `docs/AVER_NODE_NODES.md` does not document six node types that exist: `SaveGame`, `LoadGame`,
  `SetSkeleton`, and three Synapse nodes.
- `docs/STATUS.md` says so itself, but to repeat it here: its numbered feature sections stop 146
  commits ago and describe none of this release. What it does not claim to have, check first.

### Measurements

**The frame got about twice as fast.** Release build, ElectricDreams, windowed at the editor's default
1600x900, `--no-vsync`, `--frames 200`, whole-frame median, on the same machine and the same GPU
(RX 7800 XT) the 0.3.0 numbers were taken on — so the two columns are directly comparable. The
relative ladder should hold anywhere; the absolute milliseconds are that card's.

| ray tracing | 0.3.0 | 0.4.0 |
|---|---|---|
| Off (`--no-rt`) | 11.86 ms | **5.01 ms** |
| Low (1 ray, tile 4) | 18.26 ms | **7.90 ms** |
| **Medium (1 ray, tile 1 — no denoiser, the default)** | 18.44 ms | **8.15 ms** |
| High (2 rays, tile 1) | 19.99 ms | **10.08 ms** |
| Epic (4 rays, tile 1) | 23.39 ms | **13.54 ms** |

Two separate wins are stacked in that table. The base frame more than halved, 11.86 to 5.01 ms —
most of that is a single change to how fog composites, which had been marching a full sky per pixel
and then discarding it. And ray tracing itself got cheaper: turning it on at the default rung cost
6.58 ms in 0.3.0 and costs 3.14 ms now, which is mostly the ray-tracing geometry table becoming
per-mesh instead of per-instance.

Reproduce with:

```bash
build-release\bin\Sandbox.exe "<path>\ElectricDreams.ocproject" --open-legacy --frames 200 --no-vsync --frame-time --rt --rt-rays 1 --rt-pixels-per-ray 1
```

**Three things will silently give you a meaningless number instead**, all of which cost time here:

- **The project path is positional.** There is no `--project` flag. Pass nothing and the run scores
  an empty editor at plausible-looking milliseconds — 2.95 ms, in this case, which is faster than
  every row above and measures nothing at all.
- **`--open-legacy` is required** for a project stamped with an older series. Without it the run
  stops at an upgrade modal, which again scores an empty editor.
- **Read the log before believing any number.** `scene walk ... over 14 entities` is the honest
  baseline for this scene. `over 0 entities` means nothing loaded.

One caveat stated plainly: at the default camera position the chunk streamer reports the camera
outside the generated band, so terrain chunks are not resident in any of these runs. That was
equally true of the 0.3.0 measurements — same scene, same camera, same 14 entities — so the
comparison holds, but neither column is a measurement of a fully streamed world.

## [0.3.0] — 2026-08-18

The release where Aver Node stops being a demo. A graph can now aim, remember, possess and jump — the
things a first-person game is made of — and the node editor can finally open the graphs people
actually write.

### Aver Node

- **Variables are first class in the editor.** `VAR` records are modelled end to end: a Variables
  panel declares, renames, retypes and deletes them, and `GetVar`/`SetVar` pick from a list instead of
  a free-text field. Renaming offers to rewrite every node addressing the old name rather than
  silently orphaning them. Previously, dragging `SetVar` out of the palette produced a graph that
  could not compile — validation rejects an undeclared variable, and nothing in the editor could
  declare one.
- **`GetForward`** — a character's real look direction *and* eye position. `AverCharacter` keeps its
  yaw and pitch private and writes a Quat to the scene, which neither field reader can see, so a graph
  could drive a look and had no way to ask where it ended up pointing.
- **`GetViewEntity`** — the camera node a character looks through. A first-person viewmodel parents to
  this, not to the pawn; parented to the pawn it stands still while the camera pitches around it.
- **`Jump`** — declines in mid-air on its own, so wiring it straight to a key gives single jumps and no
  flight. Reports whether the jump actually happened, which "the key was pressed" is not.
- **`Raycast` reports the scene entity it hit**, not a raw physics body handle. A graph could learn
  *that* it hit something and never *what*; spawn-and-remember was the only way to know, and it is no
  longer needed.
- **`CLASS … GameMode pawn= controller=`** — a graph game mode names its pawn and controller, and
  begin-play spawns and **possesses** them. Without possession the camera follows nothing, so a
  graph-only project rendered from a default camera however loudly its character asked for first
  person. Both attributes are required together, and naming only one now warns.
- **`CLASS … view=firstperson|thirdperson`** on a Character-parented class.
- **A missing input pin is refused by name.** One hand-written `PIN` record suppresses a node's
  defaults, so omitting one left the emitter asking for a pin that no longer existed — and both
  compilers produced IL the runtime refused, reported as "Common Language Runtime detected an invalid
  program", naming neither the node nor the pin.
- **`ConstFloat value=100`** compiles to 100.0, not 0. Literals are now parsed by the node's declared
  type rather than by the shape of the text.

### Node editor

- **It can open real graphs.** `ENTRY` and `OUT` may name a node declared later in the file — the
  style every graph here uses. The C++ reader validated that inline while the C# reader deferred, so
  the editor could not open the sample projects, the demo, or any template graph.
- **Saving keeps every record where its author put it.** Records are matched individually rather than
  a whole kind being re-emitted at that kind's first line, which used to haul every `NODE` up to the
  first one and separate it from the comment explaining it. Line endings are preserved too.
- **Panning by right-drag**, alongside the existing Space-drag and middle-drag, with click and drag
  separated by a movement threshold so right-click still opens Add Node.

### Rendering

- **Ray-traced sun shadows are on by default**, at Medium, **with no denoiser**: every pixel traces
  every frame — no tiling, no reprojected history, no temporal blend. A denoiser hides its own
  artefacts as readily as the tracer's, which is the wrong default for a renderer being evaluated.
  The two knobs behind the tier now derive from it, so Medium cannot silently run Epic's cost.
- **Lighting no longer trails the camera.** Both halves of that had the same shape — a cheaper rung
  reusing last frame's work — and both were on the default tier. Shadows reused a reprojected history
  sample for three pixels in four, and the GI volume was re-voxelised only every fourth frame, so
  indirect light lagged movement by up to three. Medium now traces every pixel and re-voxelises every
  frame; measured, neither costs anything on the scene it was benchmarked against, and Low keeps both
  amortisations for anyone who wants the frame time back.
- **`--no-rt`** reaches the Off rung from the command line. `--rt` cannot express it — `0` is that
  override's "not given" — so the moment ray tracing became the default, the baseline row of the
  published cost ladder stopped being reproducible by the flags that produced the other rows.
- **Path tracing can be switched on from the editor.** The Project Settings control existed but was
  wired to a capability query that answered "not implemented" unconditionally, leaving it permanently
  greyed and the setting clamped off on every device.
- **The path-traced reference view is lit by the sun**, not only the sky, so an interior no longer
  renders honestly black.
- **DXR no longer depends on a window.** Acceleration structures initialised from swapchain creation,
  so every headless run reported no ray-tracing support directly beneath a capability line saying
  tier 11.

### Projects and the editor

- **Saving a level no longer deletes its graph classes.** Placements were rebuilt from scene
  components, which a `class=` placement does not have — so opening a level and saving it removed
  every graph class in it. For a graph-only project that is the entire game.
- **A project with no C# is no longer told it needs a `.csproj`.**
- **A project that failed halfway through creation can be created again.** The scaffolder refused an
  existing folder and then created one, so any failure after the first directory left a folder behind
  and made that project name permanently unusable.
- **New Project can start from a template**, with a `.octemplate` manifest format; scaffolding copies
  `Content/` byte for byte and never rewrites class or graph names.
- **The engine ships one: First Person.** A playable character with a CC0 viewmodel, three scorable
  targets, a game mode and a controller — five Aver Node graphs, one map, and **no C# anywhere**. It
  is the demonstration that the vocabulary above is enough to build with, not just to read about; a
  project made from it contains no `.cs` file and none is generated for it.
- **A 0.2 project opens in 0.3.** Nothing on disk needs repairing — every project-visible change this
  release is additive — but the upgrade chain reports a missing series rather than skipping it, so a
  0.1-era project would otherwise have failed to open at all.

### Removed

- **`AverGame.exe` and the packaged-game path**, including the staging and verification scripts and
  the editor's Package Project item. A second host that rendered a different subset of the scene was a
  standing source of confusion about what "the game" shows. The editor is how a project runs; the
  runtime library survives, driven by its tests.
- **The old bundled starter.** It was a project that happened to live in the engine tree, with no
  manifest, no description and no way to choose it. What replaces it is not the same thing wearing a
  new name: First Person is declared by an `.octemplate`, listed in New Project, and scaffolded
  through a path that copies content verbatim.

### Known issues

- **Global illumination does not clamp what it gathers.** Enough large, saturated, brightly-lit
  geometry and the bounce runs away and floods the frame with that surface's colour. Turning ray
  tracing on makes it easier to reach, because it lights the offending geometry more brightly. Not
  hardware-specific.
- **No 2D HUD from a graph.** Nothing in the node vocabulary draws text or 2D, so a game's score can
  only be read from the Output Log.
- **Firing is level-triggered.** Holding the fire key fires every tick unless a graph implements its
  own cooldown. The edge-triggered "pressed this frame" call exists at the ABI but no node reaches it.

### Measurements

Release build, ElectricDreams, windowed at the editor's default 1600×900, `--no-vsync`, `--frames 200`,
whole-frame median, on one machine with one GPU (RX 7800 XT) — the relative ladder should hold
anywhere, the absolute milliseconds are that card's and they move with resolution:

| ray tracing | median |
|---|---|
| Off (`--no-rt`) | 11.86 ms |
| Low (1 ray, tile 4) | 18.26 ms |
| **Medium (1 ray, tile 1 — no denoiser)** | **18.44 ms** |
| High (2 rays, tile 1) | 19.99 ms |
| Epic (4 rays, tile 1) | 23.39 ms |

The three tile widths at one ray span 0.23 ms — the same number inside the run-to-run noise — while
one ray to four costs 4.95 ms. Amortisation saturates immediately, so the temporal history was buying
no frame time in exchange for the latency it introduced. Re-voxelising GI every frame rather than
every fourth is free on the same scene: intervals 1, 2, 4 and 8 measure 18.54, 18.47, 18.50 and
18.46 ms.

Reproduce with:

```bash
build-release\bin\Sandbox.exe <path>\ElectricDreams.ocproject --frames 200 --no-vsync --frame-time --rt-rays 1 --rt-pixels-per-ray 1
```

The `.ocproject` path is **positional** — there is no `--project` flag — and a project whose
`CREATEDWITH` names an older series stops at a modal. Either mistake leaves the run scoring an empty
editor at plausible-looking milliseconds, so check the log says `scene walk … over 14 entities` before
believing a number.

## [0.2.0] — 2026-08-11

### Performance

- **The editor is roughly 30% faster on a streamed scene, and the stutter is gone.** Measured on the
  Electric Dreams demo, same camera, 870 frames, vsync off: `109.6 ms median / 215.9 ms p90` became
  `76.5 ms / 77.4 ms`. p90 now equals the median, which is the real change — frames are even instead
  of hitching every fourth one. Three causes, each of which turned out to be a feature that had never
  actually run:
  - **LOD selection was off by default.** Every instance drew LOD 0 at every distance — the exact
    thing the Cook builds a ladder to avoid. Now on; `--no-lod-select` restores the old behaviour.
    This alone is 26 ms of the 33. (`d5f2829`)
  - **The GI rebuild gate had never once passed.** `rhi::SkyAtmosphere::cloudTime` is an accumulating
    clock and sits inside the struct the gate compares byte-for-byte, so it reported "the sky changed"
    on every tick and rebuilt the whole 128³ volume forever. 77% of rebuilds are now skipped, which is
    the entire p90 fix. Found by making the rejection say WHICH BYTE differed rather than that
    something did. (`3b42099`)
  - **The depth-only passes had no LOD at all.** Four shadow cascades, the GI shadow map and
    voxelisation each drew full-detail meshes — six times a frame. They now draw a coarser proxy,
    resolved through a function pointer so the renderer still does not know Trifactor exists.
    (`d5f2829`)

### Fixed

- **Every project a shipped engine created was born unable to build.** On a Launcher install a
  brand-new project failed its first compile with `FS0039: The namespace 'Pcg' is not defined`, in a
  file the author had never opened. `payload.allowlist` shipped `scripting/csharp/**` — with a long
  comment explaining why that is load-bearing — and never `scripting/fsharp/**`, while
  `ProjectScaffold` writes an F# starter that references `Aver.Pcg`. Every guard did its local job:
  the reference walk correctly returned empty, the `<ProjectReference>` was correctly omitted, and
  `Sky.fs` was written anyway still saying `open Aver.Pcg`. Fixed at both ends — the F# tree ships,
  and the scaffold no longer writes F# it cannot reference. (`0a9c959`)
- **The GPU per-cluster mesh-shader path had never executed.** `onInit` resolved the enabling flag
  *after* `applyProject`, which is what builds the GPU cluster buffers and creates the pipeline, and
  `ensureLodMeshPipeline` latched "already tried" before testing the flag — so one early call burned
  the only attempt the process would ever make. Per-cluster frustum and cone culling have therefore
  never run in this editor. Fixed, but left **off by default**: with the path genuinely live it is 22%
  faster and renders every plant as a black silhouette, because `PSClusterMain` never receives the
  per-draw material binding. The feature is unfinished rather than merely unreachable, and the log
  line now says so. (`4434ef6`)

### Added

- **Project upgrades.** Projects record the engine version that made them (`CREATEDWITH`), which is
  distinct from `ENGINE`'s minimum-version floor. Opening a project made by an older *series* raises a
  prompt: **Upgrade a Copy** (the default — copies to a numbered sibling folder and never touches the
  original), **Convert in Place** (type the project's name to confirm), or **Cancel**. Only
  major.minor gate: `0.1.0 → 0.1.7` opens with no prompt, `0.1.x → 0.2.x` migrates. A project that
  records no version is adopted as current and stamped silently. (`7ccf87f`, `4fd31c3`, `e60926c`)
- **`Aver.Upgrade`**, a migration *chain* rather than a one-shot fix. Each step declares the series it
  moves from and to, and a path is planned through them, so a project from any past version reaches
  the current one and each step only ever knows its own boundary. Refusing is a feature: a project
  from a newer series fails with a reason rather than inventing a downgrade, and a gap in the step
  table is reported rather than skipped. (`7ccf87f`)
- **The start screen scans the projects folder**, not only the recent list — "recent" is per-machine
  state in AppData and projects are not, so a project copied from another machine or surviving a
  reinstall now appears. Each card carries its engine version in the corner; older ones are
  highlighted. (`e60926c`)
- **Editions.** `AVER_EDITION` selects a module preset the Launcher can install — `standard`, `slim`
  (no voxel GI, geometry cooking, deformation or upscaler) or `full`. The distribution side has been
  edition-aware since it was written and had only ever cut one; this is the missing half that says
  what an edition contains. `standard` reproduces the previous defaults value for value.
  (`0edb5ec`)
- **Per-pass GPU timestamps** in the D3D12 backend, riding the markers that already bracket every
  pass and read two frames late so the measurement never creates the stall it reports. The frame now
  attributes to 0.2 ms unmarked. This is what finally ended five rounds of guessing at where the
  frame went. (`1fa6cd1`)

### Tooling

- `tools/RelodTool` reports the LOD ladder Trifactor would build today for an already-cooked
  `.ocmesh`. The demo ships 69 cooked meshes and no source assets, so a simplifier change could not
  previously be evaluated at all. It reports only — rebuilding a ladder in place is a lossy rewrite of
  somebody's art. It immediately killed two cook changes that looked right: `meshopt_SimplifyPrune`
  (0.5% on the worst mesh) and lifting `target_error` to `FLT_MAX` (does not terminate). (`57dab61`)
- Both gate baselines re-recorded: 153 values moved in each config, none added or dropped, verified
  green against the binaries they were recorded from over 328 checks. (`839c24b`)

## [0.1.2] — 2026-08-05

### Fixed

- **Fog and the sky dome's lower hemisphere now blend toward the true ray's atmospheric colour at
  large scale, smoothly, instead of a flat zenith reference at every distance.** 0.1.1's fix was
  correct for the bug it fixed — a bright band and a brown patch at the horizon — but it was a clean
  break: fog always returned the same flat colour regardless of distance or how much real atmosphere
  actually lay in the way. Now blends toward the true view ray's colour, weighted by the same
  aerial-perspective transmittance already computed for ordinary surfaces — continuous, no threshold.
  (`bac1d87`)
- **The fog's own colour now responds to the sun at the range fog is actually visible at, not only at
  kilometres.** The fix above alone wasn't enough: real Rayleigh/Mie extinction only moves visibly
  over tens of kilometres, while a level's authored fog density is normally tuned to read as fog
  within a few hundred metres to a couple of kilometres — so at every distance fog actually does
  anything, the previous fix's blend sat at its unmoved end, and fog never visibly reddened at
  sunset. Fixed by asking a different question: fog scatters whatever light is actually reaching it,
  which depends on how far the sun's own light travelled to get here, not on how far the camera is
  looking. Measured: the same foggy scene reads warm gold with the sun near the horizon and cool
  neutral grey with it high overhead, at ordinary gameplay range. (`d24c979`)

### Tooling

- `scripts/publish-release.ps1`'s feed-resolve check was misdiagnosing its own bug as a GitHub
  propagation delay across two commits (`061a9e2` widened a retry window that didn't need widening)
  before the real cause was found: GitHub serves the feed's `index.json` as
  `application/octet-stream`, and `Invoke-WebRequest` sometimes hands back the response as a raw
  byte array instead of decoded text depending on that content type. Piping bytes into
  `ConvertFrom-Json` doesn't throw — it silently parses nonsense, reading exactly like "the release
  isn't live yet" when it always was. Fixed by decoding explicitly. (`c712e1f`)

## [0.1.1] — 2026-08-04

### Fixed

- **Fog and the sky dome's lower hemisphere, under a physical sky, now derive their colour from the
  atmosphere itself.** They used to come from a separately-authored gradient and a raw, unfiltered
  sun exposure — disagreeing with the physical Rayleigh/Mie model the dome is actually drawn with, so
  ground haze and the horizon read as a visibly different blue than the sky directly overhead. This is
  what Unreal's "Sky Atmosphere affects fog inscattering colour" does. (`6cf7460`, `c28d125`,
  `0a18d23`)
- **A float32 precision bug in the sky's ray/planet intersection turned the world below Z = 0 into
  visible speckle and concentric arcs.** The intersection computed `r0² − R²` directly; both terms
  are ~4×10⁷ at Earth scale while the quantity actually needed is a few km² near ground level, so the
  subtraction was mostly rounding error. Rewritten as a factored form, `(r0−R)(r0+R)`, that keeps its
  precision at any altitude. Camera altitude is also now floored at a metre rather than zero, since a
  camera exactly *on* the planet surface degenerated the same intersection a different way. (`01c141c`)
- **The dome's ground disc, under a physical sky, no longer renders as a flat lit patch that
  disagreed with the sky around it.** It now returns the atmosphere's own zenith colour, attenuated
  by transmittance over the distance to where the ground would have been — continuous with the
  horizon on one side and the fog's colour on the other, no seam. (`01c141c`)
- **Neither the packaged game nor the editor's Play-In-Editor session adds an invisible ground plane
  at Z = 0 anymore.** Every level used to get one whether it authored a floor or not, so a pit, a
  chasm, or open water below a level's own floor silently caught the player at the same height a
  level with nothing below it would. A level's own placements now supply all collision. (`6cf7460`)
- **The editor now draws the clouds a level actually asked for.** A project's sky field
  (`PCGVOLUME name Sky`) already reached the packaged game; the editor only ever carried the record
  through a save without reading it, so a brand-new project — scaffolded with exactly that record —
  opened onto a bare gradient in the editor with no clouds until the checkbox was found by hand.
  (`0a18d23`)
- **Saving a level in the editor deleted every `PCGVOLUME` record it carried**, including the sky
  field a new project is scaffolded with and the one the forest generator writes — silent data loss
  on the two levels the engine itself produces. (`662eac5`)
- **Actor editor:** the rotate gizmo turned about a different axis than the one dragged whenever the
  object already had non-zero rotation (`716a0cd`); the scale gizmo had the same defect for scale
  (`adf0e30`); saving a multi-class actor file corrupted every class after the first, because each
  class's byte-offset edits were applied against text a previous class's edit had already resized
  (`adf0e30`); mesh paths and materials containing a backslash or quote wrote invalid C# escapes back
  to disk (`adf0e30`); the actor preview's colour target was never cleared between selections, so an
  actor with holes left a trail of every earlier frame (`adf0e30`).
- New projects now open on SkyForge's own sky by default — the template previously carried Mie
  scattering strong enough to visibly diverge from the sky the engine is actually tuned against — and
  stop incorrectly reporting themselves as out of date. (`e33f9bc`)

### Added

- **Escape now stops Play-In-Editor**, the same action as clicking Stop.
- **A project's sky can be authored as an F# PCG graph the project owns**, instead of only as literal
  numbers typed into a level file — three additive entry points on the framework ABI (minor version
  1 → 2), read once a frame; a project with no sky script renders exactly what its level authored.
  (`e09691a`)
- `IRenderContext::clearColor`, needed to fix the actor-preview trail above; one production
  implementation (D3D12) and two test-mock implementations. (`adf0e30`)

### Tooling

- `scripts/publish-release.ps1` — packs a staged payload, verifies it, installs it through the real
  Aver Launcher's own code (not just the publisher checking its own manifest) and boots the result
  headlessly, then uploads. Rebuilds `averdist` and `averlauncher-cli` fresh on every run rather than
  trusting whatever is already built, after a stale binary was caught silently producing an obsolete
  pack layout.
- Gate baselines re-recorded against this tree (Debug and Release, all nine configurations). 12 of the
  18 baseline probes move by exactly +1 in one or two colour channels — the direct, measured
  consequence of the fog/sky fix above, confirmed by re-gating the pre-fix shader mid-development:
  all 18 passed unchanged there. Nothing else moved. (`cc84643`)

## [0.1.0] — 2026-08-03

First public build. The editor, the D3D12 renderer (voxel-cone-traced GI, cascaded shadow maps, DXR
inline ray-traced sun shadows, a physical sky), the scene and actor framework, physics, audio, the
C#/F# scripting layer with hot reload, and the asset importers.

### Fixed

Twenty-four verified defects (see `docs/BUGS.md` for the full list with triggering inputs); the ones
most likely to have been visible:

- Near-vertical rotations were saved wrong — the editor's quaternion-to-Euler conversion corrupted
  yaw within 0.26° of vertical, up to a full 180° flip, and level saving went through that path.
- The game runtime rendered every frame with the previous frame's camera — frame constants uploaded
  before the camera was set, so the image lagged the view by a frame while culling used the current one.
- Mouse-look was inverted, and the character's reported look direction disagreed with where the
  camera actually pointed, so aiming and traces disagreed with the picture.
- A realtime audio use-after-free: voice stealing could take a voice while the audio thread was
  rendering it, freeing sound data mid-read.
- Five buffer-overflow checks of the form `offset + length > size` overflowed when both operands came
  from a file — sitting at the front door of every binary asset format.
- Normals ignored non-uniform scale, mis-shading anything scaled unevenly on more than one axis.
- Destroying an actor during a tick silently skipped the next actor for that frame.
- A failed file watch reported itself as healthy, so hot reload stopped without saying so.

### Known limitations

Unchanged since: Windows and D3D12 only. FBX is not supported — use glTF/GLB, OBJ, or ASCII USD
(`.usda`); binary USD (`.usdc`) and `.usdz` are detected and refused by name rather than silently
importing nothing. USD import covers static meshes only — references, payloads, variant sets,
instancing and materials are not composed. No draw-call instancing yet, so dense scenes cost one draw
call per visible object.
