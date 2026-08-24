**Aver Engine 0.4.0 Beta.** Pre-1.0, and every release is a beta.

Install through the Aver Launcher, which reads this release directly, or take the zip below to unpack by hand.

---

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

- **There is no packaged-game path.** `AverGame.exe` was deliberately removed in 0.3.0 and nothing
  replaced it. The editor is how a project runs. Anything in the documentation describing a
  standalone build is describing something that no longer exists.
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
