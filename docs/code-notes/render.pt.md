# Code notes: render.pt

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/render.pt/include/aver/pt/PathTracer.hpp

- `PtSurface::albedo`: When a baseColorTex is bound, the sampled texel multiplies this factor. A caller that supplies a texture must pass baseColorFactor here and NOT the texture's mean, because mean x texel applies the texture twice, reading as a too-dark scene with no assert and no log.

- `PtSurface::baseColorTex`: Must be obtained from PathTracer::residentTexture(), never by counting draws. An index that moves when the visible set changes moves PtSceneView::drawsKey() with it and causes the accumulator to re-arm every frame, never reaching sample 1.

- `PtSurface::roughness`: Default of -1 is kept for backward compatibility with existing furnace measurements, which are an ABSOLUTE claim about an albedo-1 surface reading exactly L. A dielectric specular lobe -- even a 0.04 one -- is extra energy leaving that surface and would silently change all existing furnace measurements if the default changed.

- `PtSurface::ior`: No real dielectric has IOR of exactly 0 (vacuum/air is 1.0). The field's own absence at 0.0 signals "not a dielectric" without needing a separate flag or quantisation. This design preserves the HLSL mirror (PtInstance) from growing the ABI and losing precision.

- `PtDispatch::rouletteDepth`: Off by default because PtFurnaceTest is an ORACLE that reads back the ESCAPED FRACTION and predicts L times that fraction. A rouletted path neither escapes nor hits, so the escaped count falls while the radiance estimate stays correct, breaking the oracle's prediction for a reason unrelated to the BRDF it exists to check.

- `PtDispatch::legacyEnvironment`: Path tracer is meant as pt-compare's REFERENCE. Previous sky handling was 8x dimmer than the raster's sky due to SH-sourced calibration differences. legacyEnvironment=true restores the old, unmatched behaviour for comparison only.

- `PathTracer::residentTexture`: 4096 slots against a scene's distinct materials is not a budget anyone reaches. The table is append-only and never freed because the index travels into PtSceneView::drawsKey(); if an index could be reused or renumbered, a scene whose visible set merely changed would hash differently and re-arm the accumulator every frame.

- `PathTracer::Instance`: 88 bytes originally, with the last four bytes as a spare u32 pad. Reinterpreted as float IOR to avoid adding a new field and growing the ABI. This ABI is hand-mirrored into HLSL with nothing but a static_assert on its size watching, so each edit is a chance to shift a field on one side only.

- `energyLut_`: Integrated with the shader's own estimator rather than taken from a published analytic fit. Published fits are for a height-correlated Smith with a particular k and no below-horizon rejection; this sampler uses k = a/2 and DOES discard samples whose reflected direction falls under the surface. A fit would therefore compensate for a slightly different lobe than the one being corrected, and the furnace's 5e-3 tolerance is tight enough to see the difference.

- `tlasPool_`: A POOL INDEXED BY SCENE ORDINAL, not one shared handle. The first version of the reuse fix kept a single `tlas_` and assigned it to EVERY Scene, which is correct only with one scene at a time. PtFurnaceTest holds FIVE scenes simultaneously (an open quad, a half-albedo quad, a five-walled cave, and two dielectric quads), and every one aliased the same acceleration structure: whichever scene was built last was the scene every configuration actually traced.

- `meshRows_/blas_`: GEOMETRY IS PER MESH, NOT PER SURFACE. Deduplicating within a snapshot is critical for performance. Measured on a real level with procedural foliage: one snapshot had 2174 surfaces drawn from a handful of distinct meshes. Per-surface approach required 2174 createBlas calls, each allocating its own acceleration structure AND scratch buffer, with 31.2 MILLION vertices copied into the flat table for perhaps a fiftieth of that much distinct geometry. Per-mesh approach reduced this dramatically.

- `blasCache_`: BLAS BY MESH, SURVIVING resetScene(). A BLAS describes one mesh and stays valid until that mesh is destroyed. The RHI already tears one down at exactly that moment -- destroyMesh calls destroyBlasForMesh -- so the only way a cached handle can dangle is a mesh that died, and IDevice::meshGeometry answers false for exactly those.

- `geoMeshes_/geoVerts_/geoIndices_`: THE GEOMETRY TABLE BY MESH SET, SURVIVING resetScene(). What actually changes between snapshots is which INSTANCES are visible. The distinct meshes and their row order are usually identical. When they are, verts_/indices_ already hold exactly the right bytes, so both the allocation and the copy are pure waste. prepare() compares the freshly computed order against geoMeshes_ and reuses on a match.

## modules/render.pt/src/PathTracer.cpp

- **Resource binding tier decision**: The untextured integrator uses Tier 1 (fixed five SRVs and one UAV) to avoid raising the engine's minimum floor below DXR 1.1 hardware. The textured twin requires Tier 3 (descriptor indexing) but only the textured pipeline checks it; they are separate pipelines because a root signature is not something a dispatch can opt out of. The check is `rtBindlessTextures` (rayTracingTier >= 11 && resourceBindingTier >= 3), same as VoxiRenderer's ray path, not a bare resourceBindingTier read so the two features cannot drift.

- **Energy LUT endpoint-inclusive indexing**: Measured issue fixed by changing from texel-centre sampling to endpoint-inclusive (cell 0 exactly 0.0, cell D-1 exactly 1.0). With centres, query at roughness 1.0 (where fully rough materials land and the furnace asks) read E integrated for a smoother surface. The white-conductor check came back 5.4% short (0.9465 against required 1.000) while interior points passed. The corners of the table are real query points and must be sampled exactly.

- **BLAS deduplication**: When two surfaces on the same mesh would build separate BLAS structures, it was pure waste per geometry. Voxi's MeshHandle -> BlasHandle map often has these already built. Measured on Sponza: 220 meshes, 154.3 ms of createBlas at load, all duplicating structures over byte-identical geometry, plus double the resident BLAS memory.

- **TLAS pool lifetime**: TLASes cannot be freed (no destroyTlas in RHI), so they leak to device lifetime. The pool reuses TLAS handles per scene slot (not per-snapshot) by growing to next power of two; this O(log n) approach per slot avoids O(n) leaks on re-arm, which used to happen whenever the visible set changed.

- **GPU timing history**: The path tracer had no ScopedGpuStat until accumulate(). Until then the sole timed span in render.pt was on PtSceneView's present blit, so --gpu-timing could report show-image cost but nothing about tracing. All "path tracer is Nx faster than raster" figures were therefore whole-frame CPU numbers (from --frame-time) compared against a GPU pass span from the renderer, a category error that survived because nothing here was measurable.

- **Sky environment field**: trace[2] field in FrameCB; 1.0 keeps pre-fix unmatched-reference-sky behaviour, 0.0 (default) switches to the new behaviour. All-zero FrameCB (callers from before this field existed) already read 0.0, so PtFurnaceTest (which never sets it) is unaffected by construction.
