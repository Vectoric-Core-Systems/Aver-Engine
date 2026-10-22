# Strand hair (wave 11)

Strand hair on Jolt 5.6's Hair: a groom asset, a procedural generator, glTF curve import, the physics layer (settings, instances, wind, a compute host
table) and, in later phases, the scene system, the ribbon renderer and the shading model. Design and decisions: `docs/plans/wave11/SOFTBODY_HAIR.md`
part B. Cloth and ropes are in `docs/SOFTBODY.md`.

**What is built today (phases E to K).** The physics ABI and its CPU backend, the `.ochair` asset, the generator, curve import, proxy hull points and
the settings builder (E, F); the RHI compute host and Jolt's kernels on it, `CHair` and `HairSystem`, the ribbon renderer, `AVER_MODEL_HAIR`, the
Hair component in both hosts and the HairLab test content (G, H); the framework relay, the C# `Entity` hair members, the `Hair` node family, the Groom
tab and the Show > Hair overlay (I); ray-traced hair as an option, the shadow and acceleration-structure exclusion, the ray-hit material mask, the
`Hair` GPU timer region (J); and the Vulkan parity review, the payload checks and the headed checklists (K). Nothing was compiled or run when this was
written: every number is an UNVERIFIED starting value for the first pass, and the headed checks (section 4i) are the owner's.

## 1. Modules

| Module | What it owns |
|---|---|
| `Aver.Physics` (`physics_hair_abi.h`) | Jolt's `Hair`, `HairSettings` and `HairShaders` inside the DLL; the compute host table; the wind kernel. Still links Jolt PRIVATE. |
| `Aver.Formats` (`OcHair`, `GltfImport`) | the `.ochair` container and the glTF line-strand reader |
| `Aver.Hair` (`modules/hair`) | the generator, curves to strands, proxy hulls, the settings builder. Core, Formats and the physics C ABI only |
| `Aver.Hair.Scene` (`modules/hair.scene`) | `CHair`, `CHairDraw`, `HairSystem`: grooms shared by asset, the proxies, per-tick feeding, cadence and level of detail, the views the renderer reads |
| `Aver.Render.Hair` (`modules/render.hair`) | `RhiComputeHost` (Jolt's kernels on the RHI), `HairScene` (render feature), `AverHairRibbons.hlsl`, `RibbonPack` |

**Build wiring.** Hair needs Jolt's CPU compute backend (`JPH_USE_CPU_COMPUTE`, set ON in `modules/physics.jolt/CMakeLists.txt`; `HairInternal.hpp` stops the
build with an `#error` without it). `aver_phys_shutdown` calls `hairDestroyAll()` before the world goes, so no hair or compute buffer outlives the
`PhysicsSystem`. `AverAssetC hair` compiles `modules/hair/src/HairCurves.cpp` itself instead of linking `Aver.Hair`, so the asset compiler never depends on
the physics DLL.

## 2. The physics ABI (`physics_hair_abi.h`, version 1)

Units are the engine's (cm, engine axes); Jolt's are applied once inside. Hair space is the scalp mesh's space, the owning entity's model space.

```
settings_begin -> settings_strands, settings_scalp, settings_material, settings_params -> settings_finish -> a settings handle
create(settings, world16, layer) -> a hair
per tick:  set_transform, set_palette, set_wind, update(dt) for each hair, then flush() once
```

* **A settings object is shared** by every instance of a groom. `finish` splits the strands into render and simulated strands (a fraction per material,
  default 10%), links every other render vertex to the closest simulated one, attaches the roots to the scalp triangles and builds the compute buffers.
  The split is quadratic: call `finish` from a worker while the world is not stepping. It reports the largest root-to-scalp distance.
* **The scalp** is a triangle mesh skinned by up to four joints per vertex. The joint list is the groom's `SCALPJOINTS`; `set_palette` takes the engine's
  skinning matrices (model times inverse bind) in that order. Inverse binds are baked as identity, so the palette is used as given.
* **Parameter blocks.** `AVER_PHYS_HAIRP_*` (settings, 10 floats), `AVER_PHYS_HAIRM_*` (a material, 41 floats; a gradient is min, max, minFraction,
  maxFraction). NaN means "leave as is"; a short block keeps the defaults of the rest; `settings_set_material` edits a live settings object.
* **Refusals** (`aver_phys_last_error` says why): a strand of fewer than 3 or more than 255 points, two coincident neighbours, a material index above 255,
  a gradient whose two fractions coincide, a non-finite number, an unknown handle. `settings_destroy` is refused while an instance uses the settings.
* **Pinned roots.** The first vertex of every strand is pinned to the scalp whatever the inverse mass says.
* **Wind.** Jolt Hair has none. `AverHairWind` runs before `Hair::Update` in the same queue and pulls the velocity of every free vertex towards the wind
  (tips catch more), with a hash-noise turbulence and no clock. It is written with Jolt's `JPH_SHADER_*` macros: one source, compiled as C++ for the CPU
  backend (`HairWindWrapper.cpp`) and, in phase G, as HLSL.
* **Determinism.** Same binary, same call order: the CPU backend is deterministic. The GPU result is not claimed bit identical to it.

### The compute host

Jolt's DX12 and Vulkan compute backends stay off (design 2.10). `aver_phys_hair_set_compute_host` installs an `AverPhysComputeHostV1` function table
(`create_shader`, `create_buffer`, `map`, `unmap`, `release`, `create_queue`, `set_shader`, `bind`, `dispatch`, `readback`, `execute`, `wait`); an
adapter inside the DLL (`HairComputeHost.cpp`) forwards Jolt's `ComputeSystem` calls to it one to one. With none installed Jolt's CPU backend runs the
same HLSL as C++ on the calling thread (`HairRegisterShaders`, plus `AverHairRegisterShaders` for the wind kernel). The host is a property of the
world's hair: install it before the first `settings_finish` (refused while any settings object exists); `aver_phys_shutdown` forgets it.
`tests/physics/HairTest` proves the adapter with a fake host that wraps the CPU backend behind the table and must agree with the built-in one.

## 3. The `.ochair` asset

AVR1 subtype `HAIR`: `HEAD` (text), `STRN` (binary strands, optional), `SCLP` (an embedded scalp, optional). See `OcHair.hpp` for the grammar. Records:
`OCHAIR 1`, `SCALP <mesh> [SUBMESH n]`, `SCALPJOINTS`, `SIM`, `MATERIAL <i>`, `LOOK <i>`, `GENERATE`, `LOD`, `PROXY`, `MAXSTRANDS`. Unknown records and keys
are skipped with one warning; a newer `OCHAIR` number is refused. Writing, parsing and writing again is byte identical. `valid()` refuses strands outside 3
to 255 points, material indices above 255, more strands than `MAXSTRANDS` (40000), non-finite numbers, more than 16 scalp joints, a `SCALP` that is not an
`.ocmesh`, coincident gradient fractions and a groom with neither strands nor `GENERATE`.

## 4. Authoring

1. **Generator** (`Aver.Hair`, `HairGenerate.hpp`). `GENERATE` plus a scalp mesh and an optional per-vertex mask: roots by dart rejection weighted by mask,
   shape from the normal towards `style`, a part line, clump guides and followers, curl. Deterministic from `seed` (PCG32).
2. **glTF curves.** `LINES` and `LINE_STRIP` primitives become `ImportedHairCurves`; consecutive segments that share an endpoint form a strand, the glTF
   material index is the strand material, a `_WIDTH` attribute is read in metres. `hair::curvesToStrands` makes them simulatable (resamples above 255 points,
   drops coincident neighbours and strands under 3 points). A mesh instanced by several nodes yields its curves once.
3. **`AverAssetC hair <file.gltf> --out <groom.ochair> [--scalp <mesh.ocmesh>] [--scalp-asset <ref>] [--submesh N] [--joints a,b,c]`.** Writes a groom with
   the default `SIM`, one `MATERIAL` and `LOOK` per strand material used. With `--scalp` it measures the largest root-to-scalp distance and fails above
   `--max-root-cm` (2 by default): the curves and the scalp mesh must be in the same space.
4. **Settings builder** (`hair::buildHairSettings`). Drives `aver_phys_hair_settings_*` for a groom: stored or generated strands, the scalp from the groom's
   `SCLP` or an `.ocmesh` (`scalpFromMesh` remaps the mesh's bone indices to `SCALPJOINTS` by name), the parameter blocks, the root check (warn above 0.5 cm,
   fail above 2 cm).
5. **Proxies.** Jolt's Hair collides with convex hulls only. `proxyHullPoints` gives 32-point sphere and capsule hulls and an 8-point box;
   `defaultHumanoidProxies` derives a head sphere, a neck capsule, a capsule per clavicle (else shoulder) and a spine box from joint names, and reports what it
   did not find. The bodies themselves are made by `HairSystem` (a later phase) with the existing physics ABI.

## 4b. The compute host (phase G)

Jolt only records compute commands into a queue; `HairComputeHost.cpp` in Aver.Physics forwards them to the plain-C table `AverPhysComputeHostV1`.
`RhiComputeHost` implements that table over the RHI: copy-on-write blobs for the CPU-written buffers, a staging pool, a binding-set cache keyed by kernel
and buffer, at most 3 pending batches, `map(read)` answering null (the draw path reads GPU buffers directly, not through the CPU).

**Kernels.** Jolt's HLSL is compiled unchanged. `scripts/gen-hair-shader-regs.py` reads each `*Bindings.h` and writes, in `modules/render.hair/shaders`, a
wrapper `AverJolt<Name>.hlsl` (it defines the register macros from `AverHairMacros.hlsli`, then includes the vendored file) and `<Name>.regs.hlsli`;
`src/HairRegs.inc` is the table the host and `HairRegsTest` share. Registers are t0.. and u0.. in declaration order, `gContext` at b3 and
`gIterationContext` at b4; the wind kernel's own `gWind` is at b3. `--check` re-derives everything and fails on drift. The shader namespace is flat and
`aver_deploy_shaders` copies `*.hlsl`/`*.hlsli` non-recursively, so the vendored Jolt `Hair*.hlsl`/`Hair*.h`/`Shader*.h` are copied flat into `bin/shaders` by a
custom rule in `modules/render.hair/CMakeLists.txt`.

Jolt's own DX12 and Vulkan compute backends stay OFF.

## 4c. `CHair` and `HairSystem` (phase H)

`CHair` (64 bytes): the groom asset, width/density/wind/stiffness scales, the LOD bias, a teleport distance (cm), flags (`kHairNoCollision`, `NoShadow`,
`RayTraced`, `NoWind`, `Hidden`) and the out-state (`bit0` simulating, `bit1` frozen, `bit2` inert, `bit3` building, level from bit 8). A hidden child entity
with `CHairDraw` carries the mesh handle that `GameRender` substitutes for the entity's mesh (the same seam as soft bodies); it starts invisible and only
`HairScene` reveals it.

`HairSystem::tick` per entity: finds or builds the shared groom (synchronously; a failure is latched per entity and asset, except while the physics world is
not ready), feeds the scalp transform, the joint palette, wind and the stiffness scale, steps the cadence (`stepHairCadence`: every frame, every second, or
frozen, with a teleport after more than a second frozen), picks the strand-count level (`chooseHairLod`, 15% hysteresis, levels 100/50/25/12% at 300/700/1500
cm by default), moves the proxy bodies (layers 11 to 13; a convex hull made dynamic then kinematic) and publishes a `HairView` for the renderer. One
`aver_phys_hair_flush` per tick submits all the recorded work. `HairLod.hpp` is pure and header-only.

## 4d. The ribbon renderer

`HairScene` is an `IRenderFeature`. Per resident groom it owns the GPU points buffer (written by the Jolt kernels through the host), a render-positions buffer
and a vertex buffer; `CSRibbons` (`AverHairRibbons.hlsl`, RibbonCB 48 bytes) builds camera-facing ribbons into the vertex buffer in `prePass`, and the draw
goes through the ordinary mesh path with the posed-part mesh handle (`drawHandle(entity)`), so lighting, shadows, TAA and the existing G-buffer paths see
hair as a triangle mesh. `RibbonPack` is the layout builder (strand order shuffled so every LOD level is a prefix) and the C++ twin of the shader that the
tests drive.

**Coverage.** A ribbon thinner than a pixel is thinned by dropping whole strands stochastically (a hash of the strand and the frame against the coverage),
not by alpha: the hair stays opaque and early-Z friendly. The cost is a dither that TAA resolves; with TAA off thin hair flickers.

**Shading.** `MaterialDesc::shadingModel = Hair` sets `MaterialFlag_Hair` (bit 18) and `packMaterial` reuses existing slots under the flag (the 192-byte
`MaterialConstants` block and its mirrors do not change): beta in `roughnessFactor`, the tip colour in `emissiveFactor`, R weight/azimuth/shift in the coat
slots, TT/TRT in the subsurface weight and radius, tint in `subsurfaceColor`, scatter in `occlusionStrength`, variation in `metallicFactor`. In
`material_prelude.hlsl` `AVER_MODEL_HAIR = 2` is chosen from the flag and `averShadeDirect/Indirect/Split` switch to a Marschner-style model (R, TT, TRT lobes
with the longitudinal and azimuthal terms, a 0.15 floor standing in for multiple scattering). Ray hits keep `AVER_MODEL_STANDARD`.

## 4e. The Hair component in the editor and the level

`COMPONENT Hair  asset=<.ochair> width= density= lodbias= wind= stiffness= teleport= collision=0|1 shadow=0|1 windon=0|1 hidden=0|1` (a key at its default is
omitted). Add Component lists Hair on any placement with a mesh (a rigid head works; the collision proxies need a skeleton). The Details section is
`HairGroomTab.cpp` over `HairDetailsModel.hpp`. `AverAssetC hair-text <groom.txt> --out <groom.ochair>` writes a groom from its HEAD records as text.
Test content: `test-content/HairLab`.

Known gaps: only the first `LOOK` of a groom drives its material (an `.ocmat` override on the component replaces it, section 4f), the stiffness scale is
per shared groom, and an `.ocmat` override only turns into hair shading once the `.ocmat` reader carries the hair parameters (`shadingModel hair`).

## 4f. Scripting, nodes, the Groom tab and the overlay (phase I)

**The framework relay.** `framework_hair_abi.h` (version 1, `aver_fw_hair_*`) is a plain-C relay in the shape of the vehicle one: a provider function
installed by the composition root (`hair::hairProvider`, `user` = the `HairSystem`) and four ops that need more than a field write: teleport, add a
collider (the bone name travels as text), clear the colliders, and a six-int state read. Adding the component and writing its fields (asset, material,
flags, the scales) go through the generic scene ABI, exactly as `CCloth` does. The relay itself is compiled into `Aver.Framework` from
`modules/hair.scene/framework/FrameworkHairAbi.cpp`: the framework DLL links no hair code, and the hair area does not own `modules/framework/src`, so the
file sits with the module that answers it and the framework's CMake names it by path (it can move to `modules/framework/src/HairAbi.cpp` unchanged).

**C#.** `Aver.Framework/Hair.cs`: `Entity.AddHair(asset)`, `SetHairLook(material)`, `SetHairWidthScale`, `SetHairDensityScale`, `SetHairStiffness`,
`SetHairWindScale`, `SetHairWindEnabled`, `SetHairCollides`, `SetHairCastsShadow`, `SetHairEnabled`, `TeleportHair`, `AddHairCollider(shape, size, bone,
offset)`, `ClearHairColliders`, `TryGetHair(out HairState)`, and `Hair.SetWind` (the soft bodies' wind, so cloth, ropes and hair move together). Every
call returns false, never throws, in a process with no native scene. A negative scale is refused; a zero scale reads as the default 1 on the native side.

**Script colliders.** A hull on the proxy layer that follows a bone of the character's skeleton, so a held prop, a hat or a shoulder pad can catch
hair. At most 16 per character; they are kept per entity (so a rebuilt groom gets them again) and made once the skeleton can name the bone. A bone the
rig does not have is logged and dropped.

**The look override.** `CHair.material` names an `.ocmat`; the draw child then draws with the surface of that file's name instead of the groom's
generated one (`Hair/<groom id>`), so a hair material authored in the Material editor can replace the `LOOK` record per character. An id the content
index does not know keeps the groom's own look and logs once. It is read every tick, so `SetHairLook` takes effect at once. The `.ocmat` must carry the
hair shading model to look like hair. It is a runtime and script setting: the `COMPONENT Hair` record has no key for it yet, so a saved level does not carry it.

**The `Hair` node family** (`GraphHairNodeTable.cs`, `GraphNodeDefsHair.hpp`, `HairNodeTests.cs`; the generated `docs/nodes/Hair.md` follows the pass):
`AddHair` (attribute `asset`), `SetHairParams` (width, density, stiffness, wind; a negative leaves that scale alone), `SetHairLook` (attribute
`material`), `SetHairEnabled`, `TeleportHair`, `AddHairCollider` (shape 0 sphere, 1 capsule, 2 box; attribute `bone`), `GetHair` (a read: simulating,
level, render and simulated strand counts) and `SetHairWind` (the shared wind).

**The Groom tab** (`HairGroomTab.cpp`, `HairGroomEditor`): an `.ochair` opened from the Content Browser. Sections: Preview (the scalp's edges, the roots
and a few guide strands in front, side or top view, drawn with the ImGui draw list; a generated groom is previewed with at most 300 strands at the same
seed), Scalp (mesh, submesh, joints), Generator (every `GENERATE` field), Stored strands and curve import (a glTF with `LINES` strands; "use GENERATE
instead" drops the stored strands), Simulation (`SIM`), Strand materials (every `MATERIAL` field, the six gradients as a value pair and a fraction pair),
Looks (colours with a root-to-tip swatch, the lobe parameters), Level of detail (distances and fractions with the strands each level draws) and
Collision proxies (the list, with the humanoid starter set). A line at the top gives the strand count, the simulated count and the draw-buffer size
against the 96 MB budget, and lists what would stop the build (no scalp, no joints, over budget, not valid). Every number is clamped to what
`OcHairData::valid()` accepts; undo is a snapshot of the records only (the stored strands and an embedded scalp sit beside it), so importing or
dropping strands is not an undo step. The scalp is found relative to the Content folder by walking up from the groom's folder.

**Show > Hair** (`HairDebugOverlay.hpp`): the groom's sampled guide strands at rest (amber) and the collision proxies at their live pose (cyan; magenta
for script colliders), for the selected character or all. The options are a function-local static and the line mesh is owned by the header, so the host
needs one call per frame and one menu row.

## 4g. Ray tracing, shadows, GI and timing (phase J)

* **Ray-traced hair is an option, off by default (`kHairRayTraced`).** Voxi builds a BLAS for any compute-written mesh in the draw list and refits it
  every frame (`rtRefitAccel`), so a ribbon mesh would enter the acceleration structure on its own: about 15 MB of BLAS per full groom, and hair in RT
  shadows and reflections it was not authored for. The scene walk therefore tells Voxi to leave a hair draw OUT of the structure unless its component
  asks (`VoxiRenderer::setSubmitNoRay`, a sticky submit flag like the movable one; `Draw::noRay` makes the TLAS build, the key hash and the mover
  lists skip it, all three together so the mover tables stay in step). With the flag on, the hair is an ordinary dynamic instance; the BLAS follows the
  level that is drawn, so a level change builds the part's BLAS the first time it is used.
* **`kHairNoShadow` now opts out of the shadow cascades too.** The material's cast-shadow flag is only read by the RT shadow traversal; the cascade
  pass draws every opaque draw. `VoxiRenderer::setSubmitNoShadow` and `Draw::noShadow` skip the draw in both cascade loops.
* **GI.** Verified by reading, not changed: a compute-written mesh is never voxelised (`giVoxelisedDraw`, the voxelise slice loop and the GI build
  triangle count all skip `meshVertexBuffer(mesh) != 0`), so hair bounces no indirect light, like a skinned character; hair receives GI like any surface.
  A hair that is ray traced is hit by GI and reflection rays and shaded with the standard model.
* **The ray-hit material mask.** A ray hit cannot run the hair model (the per-draw model comes from the raster constants). It shades hair as a standard
  dielectric from the same material block; the slots hair reuses are already inert for it (coat and subsurface are gated on flags a hair material never
  sets; the tip colour in `emissiveFactor` is zeroed), and `averAuthoredFrom` now zeroes `metallic` for a hair-flagged material, because hair keeps its
  colour variation there. This is the one composition raster and rays share, so there is no second copy.
* **Timing.** `HairScene::prePass` opens a `Hair` GPU region (`ScopedGpuStat`), so `--gpu-timing` reports the replayed simulation batches and the ribbon
  dispatches together; the ribbon pass has its own nested `Aver.Hair.Ribbons` marker.
* **Budgets.** Refusals exist at both ends: `HairSystem` refuses a groom whose draw buffers would pass `vramBudgetMB` (default 96) at build, and
  `HairScene` refuses a draw that would pass the same budget; a groom bigger than `cpuMaxSimStrands` (64) is refused on Jolt's CPU backend.
* **TAA.** The ribbon mesh is compute-written like a skinned mesh and gets the same motion-vector treatment as one (the rigid object matrix, not the
  per-vertex motion). The expected symptom is the same as a skinned character's: a little ghosting on a fast head turn. Thin strands are dropped
  stochastically per frame, which TAA resolves; with TAA off the dither shows.

## 4h. Vulkan parity (phase K)

Nothing in the hair code names a backend: `RhiComputeHost` and `HairScene` use only the generic RHI, and the shaders are the shared HLSL. What the Vulkan
backend must get right, read from its source (UNVERIFIED until `HairSelfTest vulkan` and the checks below run):

* **Constant slots.** The kernels' `gContext` and `gIterationContext` are root CBVs at b3 and b4, the ribbon pass's `RibbonCB` at b3. Vulkan folds b1
  into push constants unconditionally and keeps the other root CBVs as descriptors mapped by an explicit `-fvk-bind-register` map (`buildRegisterBinds`);
  b3 is the free slot the neural module moved to, and the path tracer already declares b4 on a compute layout, so both are declarable.
* **Buffer layout.** The kernels read tightly packed structured buffers (a 12-byte `float3`, 20- and 28-byte structs). DXC needs `-fvk-use-dx-layout` for
  that, which the Vulkan device enables with `scalarBlockLayout`; skinning already depends on the same thing. A Vulkan device without it fails
  everywhere the skinning pass does.
* **States.** Every state the hair code asks for (`NonPixelShaderResource`, `UnorderedAccess`, `CopySource`, `GeometryRead`) has a buffer mapping
  in `toVkBufferBarrierInfo`; `Common` at the end of the frame is the full memory barrier.
* **Includes.** The vendored kernels' `.h` includes are resolved by the include handler both backends share. The DXIL blob cache is D3D12 only and keys
  on the `.hlsl` and `.hlsli` corpus, not on `.h` files: a change to a vendored Jolt header alone does not invalidate it (a Jolt update changes the
  kernels too, so this only matters for hand edits; `--clear-shader-cache` fixes it).
* **Look.** The hair model is plain HLSL in the shared evaluator (no struct ternaries, no derivatives); the Voxi-on-Vulkan baseline defect recorded in
  the parity plan (item V) limits what can be judged and is noted in the owner's result, not worked around here.
* **Packaging.** The shaders ship through `shaders/**`; `stage-payload.ps1` now checks that the hair shaders reached the staged tree when the hair
  module is on, and names the hair kernels in the Jolt notice.

## 4i. The owner's headed checks

`test-content/HairLab/README.md` has the step-by-step version. In order, D3D12 first and then `--backend vulkan`, with the owner's `editor.ini`:

1. `HairSelfTest` (and `HairSelfTest vulkan`): the same groom on Jolt's CPU backend and on the GPU host, 120 steps, with and without wind, within 0.2 cm.
2. The look (HairLab): the R and TRT highlights, no gap at the roots, no black hair when backlit, no flicker at rest with TAA on.
3. Motion: head shake, wind, the ponytail swing, the proxies; the Details row for a script collider.
4. Levels of detail: the walk away, the frozen band and the resume.
5. Ray tracing and shadows: hair absent from RT shadows and reflections by default; `Ray traced` on, the cost (`--gpu-timing`, region `Hair` and the Voxi accel
   build); `Casts shadows` off removes the hair's cascade shadow.
6. Budgets: 38 MB for the first instance, 23 MB each further one; the first build stall of the 9000-strand groom.
7. The Groom tab: open a groom, edit every section, undo and redo, save and reload; the preview follows the edits; Import curves on a glTF with strands.
8. `Show > Hair`, the Hair nodes in a graph, the script members.

## 5. Patents and licences

Searched on 2026-10-19; abstracts and claim 1 only, not legal advice. The owner runs the USPTO Global Dossier check before a release that ships hair.
Hair here is hardware triangle ribbons with dithered coverage: no tile binning, no per-tile depth clustering, no overlay composite (EA US 11,551,387 is
avoided). Jolt's rods carry a quaternion per rod, not a ghost point per edge (Autodesk US 10,185,789 reads on neither). Marschner, Kajiya-Kay, Scheuermann and
Zinke et al. are used as published ideas; Epic's hair shading talk and source were not consulted. Jolt is MIT; the notice ships through `THIRD-PARTY-NOTICES.txt`.

## 6. Tests

| Test | What it decides |
|---|---|
| `tests/formats/OcHairTest` | round trip, byte stability, refusals, forward compatibility, hand-edited text, damaged chunks, glTF line import |
| `tests/hair/HairGenerateTest` | roots on the scalp, spacing, length range, growth along the normal, fall towards style, clump pull, mask, part, determinism, refusals |
| `tests/hair/HairAuthoringTest` | curves to strands, proxy hulls, the humanoid default set |
| `tests/hair/HairSettingsBuildTest` | parameter blocks, scalp from a mesh, building a generated groom (root-to-scalp below 0.5 cm), resample, refusals |
| `tests/hair/HairLodTest` | cadence (every, half, frozen, teleport after a long freeze), level choice with hysteresis, the draw fraction |
| `tests/render.hair/HairRegsTest` | the register table, the generated headers and the wrappers against Jolt's binding headers |
| `tests/render.hair/RibbonPackTest` | the ribbon layout (LOD prefixes, determinism) and the C++ twin of `CSRibbons` |
| `tests/render.hair/HairMaterialTest` | the Hair material's packing, the clamps, the HLSL flag and model ids |
| `tests/render.hair/HairSelfTest` | HEADED: Jolt's CPU backend against the GPU host over 120 steps, with and without wind, tolerance 0.2 cm; exits 77 with no device. `vulkan`, `hw`, `debug` arguments |
| `tests/hair/HairGroomEditorTest` | the Groom tab headless (load, save, undo, every edit, the preview), the editing rules (`sanitize`, budget, advice, content paths, projection) and the overlay feeds |
| `tests/framework_hair/FrameworkHairAbiTest` | the framework relay with a fake provider: op codes, arguments, outputs, refusals, removal |
| `tests/framework_hair/HairProviderTest` | `hair::hairProvider` on a `HairSystem` with no groom built: validation, script collider bookkeeping, the state read |
| `scripting/csharp/Aver.Graph.Tests/HairNodeTests` | the 8 `Hair` node rows: wiring, pins, defaults, required attributes, IL, the never-throw rule |
| `tests/physics/HairTest` | refusals and info, droop under gravity, roots on the scalp, teleport, head motion, a convex hull stops the droop, wind monotone, CPU backend versus the fake host within 1e-4 m |

Thresholds are UNVERIFIED until the first run. The headed checks (`HairSelfTest` on D3D12 and on Vulkan, the look, LOD, shadows, budgets in
`test-content/HairLab/README.md`, section 4i) have not been run.
