# Code notes: render.hair

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/render.hair/include/aver/render/HairScene.hpp

- file header: the simulation, the components and the per-instance rules are Aver.Hair.Scene's (HairSystem); the compute host the simulation runs on is RhiComputeHost; this class owns the GPU side of the drawn instances.
- file header: SkinnedScene and SoftBodyScene are the model: a per-ENTITY mesh whose vertex buffer a compute pass writes (IDevice::createSkinTargetMesh), handed to the draw by substituting the MeshHandle (drawHandle) rather than teaching the draw path a new stream. Both hosts already do this for skinned characters and soft bodies.
- file header, PER GROOM (keyed by the asset): the ribbon layout (strands in a seeded random rank order, so any prefix is a uniform thinning), the point buffer, the zero-vertex source mesh whose index buffer the instances share, and the hair material built from the groom's LOOK.
- file header, PER INSTANCE: a skin-target mesh (the ribbon vertices), up to three posed-part meshes (the thinner levels, index prefixes of the same vertices), and a binding set (points, the host's render positions buffer, the vertex buffer) made once and never rewritten.
- file header, A FRAME: update() reconciles residency with the system's views, picks the level and fills the pass constants; prePass() replays the sealed simulation batches and then runs one ribbon dispatch per drawn instance; overlayPass() returns every buffer to Common, which the RHI requires at the end of a frame (the rule SkinnedMeshGpu's comment states).
- file header, THE DRAW CHILD: HairSystem makes it start invisible; this class reveals it once the instance has a ribbon mesh and a simulated pose, and hides it again when the system hides it. This class is the only thing that turns it on, so a device without compute shows no stray placeholder cube.
- kHairRetireFrames: the retire-during-recording rule.
- HairSceneOptions::vramBudgetMB: HairOptions::vramBudgetMB bounds the build the same way.
- HairSceneOptions::minPixels: the narrowest a ribbon is drawn.
- HairSceneOptions::maxBuildsPerFrame: new instances made per update (each creates meshes and a binding set).
- init: False leaves the feature INERT, not broken: hair is simulated by nobody and drawn by nobody, and the composition root installs no compute host (hairSystem().installComputeHost(nullptr)).
- hostTable: the composition root hands it to HairSystem::installComputeHost BEFORE the first groom is built.
- setMaterialBinder: the draw child's surface token (aver_scene_material) must be bound to the groom's material, which is GameContent::bindSurfaceMaterial in both hosts; without a binder the hair draws with the host's default material.
- drawHandle: ZERO means draw the authored (placeholder) mesh.
- inRayTracing: asked beside drawHandle; tells Voxi to leave every other hair draw out of the acceleration structure. A ribbon mesh rewritten each frame costs a BLAS refit (about 15 MB for a full groom) and would enter reflections and RT shadows, which hair is not authored for.
- castsShadow: true for an entity that is not hair, which is never asked.
- Groom::points: an Upload buffer written once (kept).
- Resident::part: kPartFailed once a try failed; levels made when first needed (kept).
- Resident::rayTraced: CHair kHairRayTraced, as of the last update.
- Resident::noShadow: CHair kHairNoShadow.
- Retired::meshes: parts first, then the draw mesh, then (for a groom) its source (kept).
- failed_: groom keys that would not build, not retried (kept).
- live_: keyed by the DRAW entity (kept).


## modules/render.hair/include/aver/render/RhiComputeHost.hpp

- file header, WHAT IT IS: Jolt 5.6's Hair records its whole simulation step as compute commands (SetShader, SetBuffer, Dispatch ...) into a ComputeQueue. Aver.Physics wraps that queue in an adapter (HairComputeHost.cpp) that forwards every call one to one to a plain-C table (AverPhysComputeHostV1, physics_hair_abi.h). This class is the other end: it RECORDS those calls and replays them into the frame's command stream with the RHI.
- file header: Jolt's own DX12 and Vulkan compute backends stay off, because they want a private queue and a private resource state tracker and cannot share a device with this renderer, so D3D12 and Vulkan parity is one code path.
- file header, THREADS: the table is called from whatever thread runs aver_phys_hair_settings_finish (possibly a worker) and aver_phys_hair_update (the main thread). Callbacks touch CPU memory only; handles are numbers, buffers are CPU descriptors, a dispatch is a record. Every GPU object is made, written and destroyed on the render thread by replay(), beginFrame() and the accessors.
- file header, HOW A STEP REACHES THE GPU: aver_phys_hair_update records ops; aver_phys_hair_flush calls execute(), which seals the open ops as a batch; the render feature calls replay(ctx) from prePass, which runs every sealed batch in order and clears them.
- file header: at most kHairHostMaxPendingBatches wait; beyond that the oldest is dropped (counted), which only happens when nothing is rendering.
- file header, buffers: made lazily at first use. Read-only (BUFFER, UPLOAD) and read-write buffers are Default-heap RHI buffers; their bytes reach them by a copy from a pooled Upload staging buffer at first use, so the binding sets that name them are STABLE and never rewritten (the rule SkinnedMeshGpu's ring comment enforces).
- file header, map(write): on UPLOAD or CONSTANT it is copy-on-write, so an op that bound the earlier contents keeps them, and Jolt may rewrite its materials and constants every update while an older step is still waiting to be replayed.
- file header, CONSTANT buffers: copied into the recorded op by value and handed to setConstantBuffer at replay (b3: gContext or gWind, b4: gIterationContext).
- file header, barriers: a dispatch that reads moves the buffer to NonPixelShaderResource, one that writes to UnorderedAccess, and a read-write bind with Jolt's EBarrier::Yes adds a UAV barrier when the buffer was already written this frame. endFrame() returns every buffer to Common (the RHI end-of-frame rule, SkinnedMeshGpu's rule).
- file header, map(read) on READBACK returns null: a GPU host cannot hand the CPU data the step has not computed yet, so aver_phys_hair_read_positions is refused with this host. Tests use recordReadback() and a standalone submission instead.
- kHairHostMaxBindings: per kernel (the widest has 13) (kept).
- kHairHostPoolBufferBytes: a staging buffer at least this big is destroyed after use, not pooled (kept).
- RhiComputeHostStats: hostBuffers is live host buffers; gpuBuffers is how many of them have an RHI buffer; droppedBatches is since init (kept).
- init: False (inert, nothing created) when the device has no resource factory or no compute shaders (kept).
- shutdown: host handles the physics layer still holds become dead (their callbacks return 0 / do nothing). Call after aver_phys_hair_set_compute_host(nullptr) or aver_phys_shutdown, never while the physics layer still has hair instances (kept).
- table(): the table is valid until shutdown(); `user` in the table points at the host's private state (kept).
- beginFrame: retires dead resources, ages staging buffers, purges binding sets that name a released buffer (kept).
- replay: pipelines compile on first use (a one-off hitch); a kernel that fails to compile is skipped with one log line and the hair that needs it stays frozen (kept).
- endFrame: call from the feature's overlayPass, after the scene pass has read the buffers (kept).
- gpuBuffer: made on demand; 0 for an unknown handle; its state is tracked by this host and changed only through ensureState().
- ensureState: records the barrier that moves a buffer to `state` (no-op when it is there). Only meaningful between replay() and endFrame().
- written: true once a replayed dispatch has written the buffer (a render positions buffer is garbage before that) (kept).
- bufferCount / bufferStride: the element count and stride the physics layer gave the buffer; 0 for an unknown handle.
- recordReadback: records a copy into a new Readback buffer (state restored) and returns it; the caller reads it after the submission has finished (IResourceFactory::readBuffer) and destroys it; 0 on failure.
- Impl: defined in RhiComputeHost.cpp; public only so the C callbacks of the table can name it; `user` in the table points at it.


## modules/render.hair/include/aver/render/RibbonPack.hpp

- file header, THE LAYOUT: Jolt gives one position per render vertex of every render strand; the draw wants per strand a row of ribbon points (two vertices each, left and right) and an index buffer joining neighbouring points.
- file header, THE LAYOUT: strands are put in a seeded random order (their RANK); points and indices follow that order, so ANY PREFIX of strands is a uniform thinning of the groom.
- file header, THE LAYOUT: the four LOD levels are index prefixes of 100%, 50%, 25% and 12% of the strands (HairLod.hpp's fractions); a level is one index range of the same vertex buffer, which IDevice::createPosedPartMesh builds.
- file header, THE LAYOUT: buildRibbonLayout is deterministic: the same strand table, widths and seed give the same layout, bit for bit.
- file header, THE TWIN: ribbonVertices is AverHairRibbons.hlsl's CSRibbons for one point, line for line (the discipline anim::skinVertices keeps with skin.hlsl); it covers the camera-facing side, the one-pixel minimum width with its coverage, the stochastic keep and the uv encoding.
- file header, THE TWIN: RibbonPackTest pins its properties.
- RibbonPoint: `vtx` is the render vertex index; `prev`/`next` are the point before/after it (itself at root/tip); `fraction` is 0 root .. 1 tip; `widthCm` is the width before scaling; `rankFrac` is rank / strand count.
- RibbonConstants: `cam` was documented as camera in hair-local engine centimetres; `pixelAt1cm` as local cm per pixel per cm of distance.
- RibbonInput: `widthRootCm` and `widthTipCm` were documented as per strand.
- RibbonLayout: `rankToStrand` was documented as strand index (render buffer order) of each rank.
- RibbonLayout: `indices` 6 per segment, same order (kept); `lodPoints`/`lodIndices` were documented as points in the first lodStrands[l] strands (the dispatch size of a level) and indices of those strands (the index count of the level's part mesh).
- ribbonMix / ribbonUnit: the hashes of the shader: hairMix is a 32-bit finaliser; hairUnit maps it to [0, 1) from its top 24 bits.
- ribbonJoltToLocalCm: hair-local engine position (cm) of a Jolt render position (metres, Y up), the inverse of Convert.hpp's toJolt, scaled.
- RibbonCamera.widthScale: CHair::widthScale times the level's boost.
- RibbonResult: widthDrawn is W, the ribbon width written in centimetres (0 when dropped); coverage was w / max(w, one pixel); dropped means collapsed by the rank rule or the stochastic keep.
- ribbonVertices: pJ / prevJ / nextJ are Jolt render positions of the point and its neighbours.
- ribbonEncodeV / ribbonDecodeV: uv.y, the strand's 8-bit hash in the integer part and the root-to-tip fraction in the rest; the pixel shader decodes it.
- ribbonPixelAt1cm: pixel footprint of the view at 1 cm for a perspective projection: 2 tan(fovY / 2) / viewport height in pixels.
- ribbonLodWidthBoost: the width boost of a level that draws `fractionDrawn` of the strands is 1 at 1 and grows as the square root of its reciprocal, capped (a 1% level would otherwise draw tape).
