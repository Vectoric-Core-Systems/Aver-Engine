**Aver Engine 0.5.0 Beta.** Pre-1.0, and every release is a beta.

Install through the Aver Launcher, which reads this release directly, or take the zip below to unpack by hand.

---

The release where the default renderer started telling the truth. Aver's default path is ray-driven
primary visibility — rays, not a rasteriser, decide what every pixel sees — and until now it shaded
each of those hits with one flat colour per material. Textures existed; the renderer that actually
paints the screen could not read them. That is fixed, and fixing it turned out to make the frame
*faster* rather than slower. Around it: 259 commits of the same shape, where the interesting part is
usually how long something had been quietly not working.

### Rendering

- **The default renderer samples real textures.** Ray-driven primary visibility shaded every hit
  with a per-material average colour; a brick wall and a painted wall were the same wall. Against a
  raster reference on ElectricDreams, mean absolute difference fell from **16.76 to 6.06** out of 255,
  and pixels differing by more than 24 codes from **30.82% to 3.65%**. Reflections and glass panes
  sample textures too, through the same table.
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
  **+0.138** against +0.002 for a low-iron control; the pool floor's shadow went (19,33,42) →
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
