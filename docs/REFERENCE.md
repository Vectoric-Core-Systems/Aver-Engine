# Aver Engine documentation: reference front page

Hand-written lookup for everything under `docs/`. First the reference material you search by name, then every
document grouped by category, alphabetical within a category. Paths are relative to this folder.

Several docs carry a status banner at the top (stale, plan only, checked on a date). Read the banner before trusting
a page; [STATUS.md](STATUS.md) and [BUGS.md](BUGS.md) say which pages are known to lag the code.

## Reference

| Looking for | Go to |
|---|---|
| C ABI (every exported `aver_*` function, by module) | [abi/index.md](./abi/index.md) (generated lookup), [ABI.md](ABI.md) (the seams, versions, rules) |
| C# API (every public type and member) | [api/index.md](api/index.md) (generated, by assembly), [api/members-a-z.md](api/members-a-z.md) (all members A-Z), [SCRIPTING_API.md](SCRIPTING_API.md) (hand-written tour) |
| Aver Nodes (visual scripting) | [nodes/index.md](nodes/index.md) (families table and every node A-Z), [nodes/README.md](nodes/README.md) (how the pages are made), [AVER_NODE_NODES.md](AVER_NODE_NODES.md) (behaviour notes) |
| File formats (`.oc*`, the `AVR1` container) | [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md), [formats/DECISIONS.md](formats/DECISIONS.md) |
| Modules (manifest, dependency rule, catalog) | [MODULES.md](MODULES.md) |
| Projects and settings keys (`.ocproject`, manifest lines) | [PROJECTS.md](PROJECTS.md) |
| Command-line flags | Editor and capture flags: [EDITOR.md](EDITOR.md) ("Driving it from the command line"); `--frames` captures and why they differ from the editor: [headless-vs-editor.md](headless-vs-editor.md); render queue: [RENDER_QUEUE.md](RENDER_QUEUE.md); `AverAssetC` and extensions: [EXTENSIONS.md](EXTENSIONS.md) ("Command line"); opening a project: [PROJECTS.md](PROJECTS.md) |
| Error codes, log levels, crash reporter | [DIAGNOSTICS.md](DIAGNOSTICS.md) |
| Code-level design notes per module | [code-notes/README.md](code-notes/README.md) |

## Getting started and status

| Doc | What it covers |
|---|---|
| [BUGS.md](BUGS.md) | Verified bug list from two sweeps, each entry with its triggering input. |
| [MINIMUM_SPECS.md](MINIMUM_SPECS.md) | Hardware requirements and minimum specs (internal reference for the launcher and store page). |
| [PROJECTS.md](PROJECTS.md) | The engine/project split: what a project holds, the manifest, how a project loads. |
| [STALE_CODE.md](STALE_CODE.md) | Stale-code sweep of 2026-08-03; its `sandbox/` citations are dead, re-resolve before acting. |
| [STATUS.md](STATUS.md) | Status and handoff log (stamped 2026-08-17, stale; long history). |

## Architecture and modules

| Doc | What it covers |
|---|---|
| [ABI.md](ABI.md) | The C ABI seams between modules: version constants, export counts, rules. |
| [ABI_VERIFICATION_PLAN.md](ABI_VERIFICATION_PLAN.md) | Design (not code) for a startup module-ABI/version check in `Aver.Core`. |
| [ARCHITECTURE.md](ARCHITECTURE.md) | The original modular architecture specification and mandate. |
| [EXTENSIONS.md](EXTENSIONS.md) | Extensions: features installed beside the exe that the engine finds by manifest, never by name. |
| [FABRIC.md](FABRIC.md) | Fabric Connector: one-way conversion of a Godot 4 project into an Aver project. |
| [GAME-LIFT.md](GAME-LIFT.md) | Lifting the game half out of `SandboxApp` into a runtime (the story, in order). |
| [MODULES.md](MODULES.md) | Module manifest (`module.json`), the dependency rule and the module catalog. |
| [RUNTIME-DEDUP.md](RUNTIME-DEDUP.md) | Reconciling the editor's private runtime copy with `Runtime/`. |
| [SCENE_FRAMEWORK.md](SCENE_FRAMEWORK.md) | `Aver.Scene` (entity/component world) and `Aver.Framework`: the design. |

## Editor and asset import

| Doc | What it covers |
|---|---|
| [ACTOR_EDITOR.md](ACTOR_EDITOR.md) | Actor Editor with live sync: preview, designer reader/rewriter, gizmo, file watcher. |
| [ASSET_IMPORT.md](ASSET_IMPORT.md) | How third-party geometry and textures get in, and what each importer supports. |
| [DESIGNER_REWRITE.md](DESIGNER_REWRITE.md) | The locked grammar for writing viewport edits back into generated regions of a `.cs`. |
| [EDITOR.md](EDITOR.md) | The in-window editor: spec, phases, and the command-line flags (mostly plan; read its banner). |
| [editor/PLAY_IN_NEW_WINDOW.md](editor/PLAY_IN_NEW_WINDOW.md) | Play options "New Window": the normal PIE session in a separate window. |
| [FBX_IMPORT.md](FBX_IMPORT.md) | Importing `.fbx`: options, mappings, what is dropped. |
| [headless-vs-editor.md](headless-vs-editor.md) | Every way a `--frames` capture differs from what the editor shows. |

## Rendering and VFX

| Doc | What it covers |
|---|---|
| [AVERSR.md](AVERSR.md) | AverSR, the upscaling subsystem: one name, several implementations. |
| [PARTICLES.md](PARTICLES.md) | The particle system, `.ocparticle` format, emitter component and editor tab. |
| [rendering/ATMOSPHERE.md](rendering/ATMOSPHERE.md) | The physical atmosphere: where the sky comes from and which numbers are measured. |
| [rendering/DECALS.md](rendering/DECALS.md) | Projected decals (box repainting base colour, normal, roughness/metallic). |
| [rendering/DENOISING.md](rendering/DENOISING.md) | Denoising with the vendored FidelityFX denoiser. |
| [rendering/FSR3_EVALUATION.md](rendering/FSR3_EVALUATION.md) | Evaluation of FSR3 frame generation as a NeuraFI replacement (2026-10). |
| [rendering/LIGHTS.md](rendering/LIGHTS.md) | Scene lights: point, spot, rectangular area, IES profiles. |
| [rendering/MANY_LIGHTS.md](rendering/MANY_LIGHTS.md) | Many lights: stochastic direct lighting from a light tree (Voxi). |
| [rendering/MOVING_OBJECTS_HISTORY.md](rendering/MOVING_OBJECTS_HISTORY.md) | Temporal history of shadows, AO, reflections and GI under moving objects. |
| [rendering/NEURAA_NRD.md](rendering/NEURAA_NRD.md) | NeuRAA (neural anti-aliasing) and NRD (neural denoising). |
| [rendering/NEURAC.md](rendering/NEURAC.md) | NeuRaC, the neural radiance cache for ReSTIR's second bounce. |
| [rendering/NEURAFI.md](rendering/NEURAFI.md) | NeuraFI, the engine's frame interpolation: design and code. |
| [rendering/NEURAFI_PATENTS.md](rendering/NEURAFI_PATENTS.md) | Frame-interpolation patent sweep for counsel (not legal advice). |
| [rendering/NRD2.md](rendering/NRD2.md) | NRD v2: single-frame neural denoiser over the composed lighting. |
| [rendering/PERF_060_VS_070.md](rendering/PERF_060_VS_070.md) | 0.6.0 vs 0.7 frame cost on NewSponza. |
| [rendering/POST_LENS_GRADING.md](rendering/POST_LENS_GRADING.md) | Post chain: lens effects and colour grading. |
| [rendering/POST_VOLUMES.md](rendering/POST_VOLUMES.md) | Post-process volumes and grading LUTs. |
| [rendering/RENDERING.md](rendering/RENDERING.md) | The rendering architecture design (original spec). |
| [rendering/UNIFIED_LIGHTS.md](rendering/UNIFIED_LIGHTS.md) | Unified lights: one system for sun, local lights and emitters. |
| [rendering/VIEW_MODES_PLAN.md](rendering/VIEW_MODES_PLAN.md) | Plan: wireframe/gizmo fixes, Unlit and Shader Complexity view modes. |
| [rendering/VOLUMETRIC_FOG.md](rendering/VOLUMETRIC_FOG.md) | Volumetric fog (froxel light shafts). |
| [rendering/VRAM.md](rendering/VRAM.md) | VRAM budget display and behaviour over budget. |
| [VIRTUALIZED_GEOMETRY.md](VIRTUALIZED_GEOMETRY.md) | Clustering, LOD hierarchies and GPU-driven rendering; per-instance LOD default. |
| [VULKAN.md](VULKAN.md) | The Vulkan backend: state, defects, parity with D3D12. |

## Materials

| Doc | What it covers |
|---|---|
| [MATERIALS.md](MATERIALS.md) | `.ocmat` files and node-graph shading (`.ocgraph` material graphs). |

## Animation and cinematics

| Doc | What it covers |
|---|---|
| [ANIM_BLEND_STATE.md](ANIM_BLEND_STATE.md) | Blend spaces and animation state machines. |
| [ANIM_IK_RETARGET.md](ANIM_IK_RETARGET.md) | IK Rig, Control Rig inputs and retargeting. |
| [ANIM_MONTAGES.md](ANIM_MONTAGES.md) | Montages, slots, layered and additive blends, root motion. |
| [ANIM_MORPH_FACE.md](ANIM_MORPH_FACE.md) | Blend shapes, facial animation and lip-sync. |
| [ANIM_MOTION_MATCHING.md](ANIM_MOTION_MATCHING.md) | Motion matching. |
| [ANIM_POSE_SEAM.md](ANIM_POSE_SEAM.md) | Script bone poses, pose-to-pose retarget, authored ragdolls, anim-driven physics. |
| [RENDER_QUEUE.md](RENDER_QUEUE.md) | Movie Render Queue: a camera sequence to PNG/EXR/MP4, with its command line. |
| [SEQUENCER.md](SEQUENCER.md) | The level sequencer: tracks, keys and playback. |

## Physics

| Doc | What it covers |
|---|---|
| [DESTRUCTION.md](DESTRUCTION.md) | Brittle (Voronoi fracture) destruction. |
| [FLUIDS.md](FLUIDS.md) | `Aver.Fluids`: a particle fluid, real-unit materials, two surface paths. |
| [HAIR.md](HAIR.md) | Strand hair on Jolt: grooms, generator, glTF curves, physics layer. |
| [physics/AREAS.md](physics/AREAS.md) | Contact monitoring and Areas. |
| [physics/CHARACTER.md](physics/CHARACTER.md) | Character additions on Jolt's `CharacterVirtual`. |
| [physics/GRAPH_NODES.md](physics/GRAPH_NODES.md) | Physics wave 1b graph nodes. |
| [physics/JOINTS_AND_BODIES.md](physics/JOINTS_AND_BODIES.md) | Breakable joints, soft limits, motor springs, per-body extras. |
| [physics/KINEMATIC.md](physics/KINEMATIC.md) | Kinematic character controller (`move_and_slide` style). |
| [physics/LAYER_FILTERING.md](physics/LAYER_FILTERING.md) | Opt-in layer/mask collision filtering beside the 16-layer matrix. |
| [physics/PHYSICAL_ANIMATION.md](physics/PHYSICAL_ANIMATION.md) | Hit reactions, partial-body simulation, death and recovery. |
| [physics/QUERIES.md](physics/QUERIES.md) | Shape casts, overlaps, every-hit rays, point query. |
| [physics/RAGDOLL.md](physics/RAGDOLL.md) | Ragdolls: capsule parts with swing-twist limits. |
| [physics/RAGDOLL_ANIMATION.md](physics/RAGDOLL_ANIMATION.md) | Ragdolls on skinned entities (`Aver.Ragdoll`). |
| [physics/RAYCAST_VEHICLE.md](physics/RAYCAST_VEHICLE.md) | Raycast vehicle and joint parameters. |
| [physics/SOFTBODY_DESTRUCTION.md](physics/SOFTBODY_DESTRUCTION.md) | Soft-body builder ABI for cloth and destruction. |
| [physics/SPACE_QUERIES.md](physics/SPACE_QUERIES.md) | Space-state queries and test motion. |
| [PHYSICS_INTERPOLATION.md](PHYSICS_INTERPOLATION.md) | Interpolating fixed-step physics for smooth rendering at any frame rate. |
| [PHYSICS_SHAPES.md](PHYSICS_SHAPES.md) | Collision shapes, physical surfaces and terrain collision. |
| [SOFTBODY.md](SOFTBODY.md) | Cloth and destruction on Jolt soft bodies. |
| [VEHICLES.md](VEHICLES.md) | Drivable cars, trucks and tanks from `.ocvehicle` data. |

## AI and gameplay

| Doc | What it covers |
|---|---|
| [ACCESSIBILITY.md](ACCESSIBILITY.md) | Player accessibility: UI scale and contrast, subtitles, colour-vision filters, input timing. |
| [AI_CROWDS_HEARING_COVER.md](AI_CROWDS_HEARING_COVER.md) | Synapse steering, crowds, hearing, cover and squads. |
| [AI_QUERIES_SMARTOBJECTS_STATETREE.md](AI_QUERIES_SMARTOBJECTS_STATETREE.md) | Environment queries, smart objects and StateTree. |
| [BLACKBOARD_BT.md](BLACKBOARD_BT.md) | Blackboard, behaviour-tree decorators, BT editor, AI debug view. |
| [CAMERAS.md](CAMERAS.md) | Gameplay camera rigs: spring arm, modes, blending. |
| [DATA_TABLES.md](DATA_TABLES.md) | `.csv`/`.json` data tables bound to C# records. |
| [GAMEPLAY_ABILITIES.md](GAMEPLAY_ABILITIES.md) | Gameplay Ability System: tags, attributes, effects, abilities. |
| [GAME_TIMERS_EVENTS.md](GAME_TIMERS_EVENTS.md) | Frame-safe timers and the typed event bus. |
| [LOCALIZATION.md](LOCALIZATION.md) | Translated text per culture, chosen at run time. |
| [SYNAPSE.md](SYNAPSE.md) | Synapse, the AI system: navigation, perception, behaviour. |

## Audio

| Doc | What it covers |
|---|---|
| [AUDIO.md](AUDIO.md) | The audio plan (now built): engine, mixing, API. |
| [AUDIO_STREAMING.md](AUDIO_STREAMING.md) | Streamed playback, music crossfades, occlusion and reverb zones. |
| [SOUND.md](SOUND.md) | Aver Sound: procedural audio authored as a node graph. |

## UI

| Doc | What it covers |
|---|---|
| [GAME_UI.md](GAME_UI.md) | Game UI widgets, menus and the layout editor. |
| [GAME_UI_TEXT.md](GAME_UI_TEXT.md) | Game UI text: TrueType fonts, fallback chains, shaping, right-to-left. |

## World and levels

| Doc | What it covers |
|---|---|
| [CHUNKS.md](CHUNKS.md) | Chunked worlds, region streaming, generation as you go. |
| [LEVEL_INSTANCES.md](LEVEL_INSTANCES.md) | Level instances and packed level actors. |
| [LEVEL_STREAMING.md](LEVEL_STREAMING.md) | Streaming authored `.ocworld` levels. |
| [PREFABS.md](PREFABS.md) | Prefabs: saved entity hierarchies, instances and overrides. |

## PCG and landscape

| Doc | What it covers |
|---|---|
| [LANDSCAPE_EDITOR.md](LANDSCAPE_EDITOR.md) | Landscape editor: feasibility survey and plan. |
| [LANDSCAPE_LAYERS_WATER.md](LANDSCAPE_LAYERS_WATER.md) | Landscape paint layers, rivers and lakes (user guide). |
| [PCG.md](PCG.md) | Procedural content generation API and the graph system on top. |

## Scripting and nodes

| Doc | What it covers |
|---|---|
| [api/index.md](api/index.md) | Generated C# API index by assembly (per-assembly pages and an all-members A-Z page beside it). |
| [AVER_NODE_NODES.md](AVER_NODE_NODES.md) | Aver Node long-form behaviour notes; pins live in the generated pages. |
| [nodes/index.md](nodes/index.md) | Generated node lookup: families and every node A-Z. |
| [SCRIPTING.md](SCRIPTING.md) | C# scripting guide: behaviour lifecycle, `Pbr` and `Voxi` APIs. |
| [SCRIPTING_API.md](SCRIPTING_API.md) | Hand-written tour of the C# scripting API. |
| [VISUAL_SCRIPTING.md](VISUAL_SCRIPTING.md) | Aver Node guide: graph classes, entry points, adding a node family. |

## Networking

| Doc | What it covers |
|---|---|
| [NETWORKING.md](NETWORKING.md) | Replication and RPCs in `Aver.Framework.Net`. |
| [physics-net/PHYSICS_NET.md](physics-net/PHYSICS_NET.md) | Physics/sim and networking module design (written for modules then unbuilt). |

## Packaging and platforms

| Doc | What it covers |
|---|---|
| [PACKAGING.md](PACKAGING.md) | Packaging a project into a shippable game (checked for 0.8.0). |
| [PLATFORMS.md](PLATFORMS.md) | Building and running on Linux and macOS. |

## Tooling, tests and file formats

| Doc | What it covers |
|---|---|
| [DIAGNOSTICS.md](DIAGNOSTICS.md) | Log severity ladder, exit codes, ABI reasons, the crash reporter. |
| [code-notes/README.md](code-notes/README.md) | Index of per-module design notes moved out of code comments. |
| [formats/DECISIONS.md](formats/DECISIONS.md) | Project-owner format directives that override the broader design. |
| [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md) | The `AVR1` container and `.oc*` format specs. |
| [Images/README.md](Images/README.md) | Images used by docs and showcase. |
| [TOOLING_COMPARISON_DRIFT.md](TOOLING_COMPARISON_DRIFT.md) | Tooling comparison against Drift Engine and the gaps it found. |

## Plans and history

| Doc | What it covers |
|---|---|
| [plans/](plans/) | Feature and wave plans: UE parity, Godot parity, first person, 2D, game systems, per-wave design contracts. |
| [recon/](recon/) | Reconnaissance notes: conventions, control rig, GPU deform, networking, soft-body solver, formats. |
