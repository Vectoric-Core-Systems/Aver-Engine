# Code notes: formats

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/formats/include/aver/formats/OcProject.hpp

- `kOcProjectVersion`: The versioning policy is strict—a file claiming a higher version is rejected outright rather than read as version 1. The policy is: bump this constant only after the parser is taught what the new version means, never ahead.

- `ProjectDesc::createdWith`: This field stores the engine version that last opened the project, distinct from `engineMinVersion` (which is a floor the manifest may state). Projects that existed before this field has no value; empty is deliberately treated as "current" and adopted on open. Only projects stamped with an older series are prompted to upgrade. This avoids spurious migration prompts for projects that may be fine.

- `ProjectDesc::droneGraph` and `ProjectDesc::inputScheme`: These are project keys (file path keys), not assumed filenames. This design exists because the engine must not assume a project contains a file with any particular name. Without these keys, the drone would spawn with no graph from the editor or drone window (appearing "glitched" because it sat still), and input bindings could not be authored as data. A project stating no value still gets consistent behaviour (drone sits still, or gameplay code builds the context directly).

- `voxelResolution`: The valid range is [32, 512] clamped by `Voxi.cpp::setSettings`. Epic tier derives 512. The 512³ RGBA16F grid with mip chain is ~1.2 GiB, so 512 is a real performance choice. Previous comment text misleadingly stated "64 / 128 / 256" which named neither the real floor nor ceiling.

- `rdStages` (ray-driven stages): The field controls the ray-driven primary's internal shape. 0 = single pass (baseline/fallback, one drawFullscreen); 1 = staged visibility → shadow → shade (D3D12 only, falls back to single pass if unavailable); 2 = staged + half-rate GI (milestone 4: checkerboards GI rays, denoiser reconstructs, deliberately changes the image vs. 1's same-image comparison). This field has no ladder rung—an absent key leaves the engine default (2) alone regardless of RT tier changes (unlike `rtRenderMode` which falls back).

- `restirHistory` (ReSTIR GI temporal history): Measured maxHistory weight impacts on Sponza: history 0 = no overshoot at rest or moving; history 1 (+8% vs. settled); history 8 (+104% vs. settled). Not tier-derived—has no ladder rung—so absent here leaves engine default (0) alone regardless of GI tier.

- `backend` (RHI backend selection): This is a preference, not a guarantee. The backend must be compiled in (AVER_RHI_VULKAN defaults OFF in CMake) and must successfully create a device on the target machine; fallback occurs if unavailable. The peek is early in `SandboxApp::main()` before `setBackend` (search for RENDER.BACKEND there). This is read before the editor opens a project—far too early to apply through the per-frame render settings path.

- `frameBudgetMs`: Used to enable quality auto-tuning to hit a target frame time. A renderer that quietly retunes itself cannot be A/B tested meaningfully, so this must be disabled (≤0) for measurements.

- `averSr` and `frameInterp`: Not applied through `ProjectRenderApply.hpp`. Render.voxi must not include render.sr (module boundary per docs/AVERSR.md), so both hosts read these fields directly. The editor's own Display > AverSR choice and CLI `--aversr`/`--frame-interp` outrank the project keys.

- UI settings (msaa, meshShaders, giUpdateInterval): These were live Project Settings controls that applied immediately then vanished on reopen—`captureRenderSettingsFromUi` never read them and no manifest key existed. Setting MSAA to 8x, saving, and reopening would reset to 4x. The asymmetry with `giUpdateInterval` was worse: the flag `--gi-update-interval` existed with no manifest key, while the engine field was tier-derived, silently re-deriving over the flag mid-session with no log.

- `hasGiVolume`, `giCenter`, `giExtent`: Two sliders in Project Settings > Global Illumination were settable in the UI but never persisted. On levels where interesting geometry is not at the origin, GI placement is critical. `hasGiVolume` is a presence flag (not a sentinel) because every component of a centre is legitimately negative. The extent is a scalar (not a vector) because the volume is a cube—the renderer holds `Vec3 giCenter_` + scalar `f32 giExtent_`.

- Post-processing settings (postExposure, postBloom, postAutoExposure, postTonemap): These were settable via CLI (`--bloom`, `--exposure`, `--auto-exposure`, `--tonemap` in SandboxMain.cpp) and the editor's Post panel but had no file persistence. Shipped games always rendered with compiled defaults (exposure 1, bloom 0.06, auto-exposure on, tonemap 2 per RHI.hpp:146-217), losing all author intent. Floats use -1 as unstated, not 0, because zero is a valid and meaningful value (bloom 0 = no bloom pyramid built, a performance choice). On PTTest Sponza at exposure 8: tonemap mode 1 holds chroma 1.41 where mode 2 holds 3.09. Mode 0 (Narkowicz/Hill) is the baseline for all recorded gate baselines in scripts/.

- `hasRenderSettings()`: Bug N9—`giMode` and `denoiser` were parsed but never applied. These fields already existed in the struct but the check `hasRenderSettings()` did not include them, so `ProjectRenderApply.hpp`'s apply block was gated out entirely if only those two keys were stated, silently ignoring `RENDER.GIMODE` 1 or `RENDER.DENOISER` 1.

- Window settings (windowTitle, windowWidth, windowHeight, windowResizable, windowFullscreen): These were hardcoded (1280×720 from `platform::WindowDesc`) with no flag, key, or UI. A shipped game had no way to set its presentation beyond recompiling. Previously, the gap was supposed to be filled by a side-car game.json that never had a reader or writer.

- Import settings (importScale, importConvertAxes, importGenNormals, importGenMips, importMaxTexture): Keys parse and serialize and are editable in Project Settings, but no importer option struct (GltfImportOptions, ObjImportOptions, UsdImportOptions, TextureLoadOptions, MaterialCookOptions) is constructed from ProjectDesc. `AverAssetC` takes its options from its own command line without opening the owning .ocproject. The design intent—to declare project-wide import defaults—was left incomplete.

- Stream settings (streamLoadRadius, streamEvictRadius, streamLoadBudget, streamEvictBudget, streamVerticalRadius, streamLeadSeconds): Applied in `GameStreaming::enable`, seeding `ChunkWorldSettings::stream` before a PCGVOLUME's own `radiusChunks` overrides the radii. These fields were parsed, written back, and editable in Project Settings while reaching nothing—keys whose only consumer is the file they came from. `loadBudget` is documented as "the only thing bounding the frame hitch, since the load is synchronous."

- Physics settings (gravity, fixedStep, physMaxBodies, physMaxBodyPairs, physMaxContacts, physTempAllocatorMb): Gravity uses a presence flag (not a sentinel like the RENDER.* keys) because gravity points down and components are legitimately negative. Physics ceilings (maxBodies, maxBodyPairs, maxContacts, tempAllocatorMb) were hardcoded in `PhysicsWorld.cpp::system.Init(4096, 0, 8192, 2048, ...)` with no flag, key, or UI—a project exceeding 4096 bodies could not be configured without recompiling.

- Audio mix (hasAudioMix, masterVolume, busVolume[]): Uses a presence flag because zero is the most meaningful value (a project that ships with music muted must be able to say so). The bus order matches `audio_abi.h`'s AVER_AUDIO_BUS_SFX / MUSIC / VOICE / UI.

## modules/formats/src/OcProject.cpp

### Version handling
- REFUSED rather than read as VERSION 1
- Number parsed, round-tripped, never compared
- Future OCPROJECT 2 would be read as if keys still meant today, quietly dropped what didn't understand
- .ocmat and AVR1 container refuse unsupported version; text formats were odd ones out
- Version field nobody checks is field that cannot be used
- Ceiling, not equality: older engine projects are migration system's business; only future unreadable



- **isOwnedKey list ordering**: The kOwned[] array in `isOwnedKey()` must match the order keys are emitted in `writeOcproject()`. A key appended to the owned list but missing from isOwnedKey will be stripped from the input but never replaced, causing it to be deleted on every save. Because owned keys are inserted at the first owned line while author's lines stay below, last-write-wins parsing makes the stale line win, so the change appeared to work but reverted on reload.

- **DRONE.GRAPH and INPUT.SCHEME as owned keys**: These are emitted even when empty because they are listed in isOwnedKey. Without this, the writer would strip whatever line the file had without ever replacing it, silently deleting the project's drone graph or input scheme reference on every save.

- **writeOcproject() always emits OCPROJECT header**: The function always returns a manifest with an OCPROJECT header, even if the input `existing` parameter had no header. This was not hypothetical: ProjectScaffold::manifestText passes three comment lines as `existing`, so the editor scaffolded every new project with no header. loadOcproject then refused it with "not an .ocproject: no OCPROJECT header line". The guard then deleted the half-made folder, so the user saw a creation that simply refused. The header is now prepended to ensure any caller (template, importer, migration) that passes a preamble doesn't have the same bug.
