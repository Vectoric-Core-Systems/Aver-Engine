# Landscape paint layers, rivers and lakes

Wave 3, feature 13. Design contract: [`plans/wave3/LANDSCAPE_LAYERS_RIVERS.md`](plans/wave3/LANDSCAPE_LAYERS_RIVERS.md). This is the user guide: formats, tools, material, scripting and limits.

## What you get

- **Paint layers.** Weightmap paint on a landscape section, up to 8 layers, stored in the `.ocland` by layer name.
- **A layered terrain material.** One `.ocmat` defines the layers (textures, tiling, physical surface); the pixel shader blends them by the paint and a height mix, in raster and in rays.
- **Physical surfaces.** The same layer data gives `Landscape.Sample` (surface, friction, restitution) to C# and graphs, and sets the friction of rigid bodies sliding on the terrain.
- **Rivers and lakes.** Spline rivers and closed-outline lakes as level records, drawn as ordinary meshes, with flow scrolling in the material, optional terrain carving, and buoyancy that reads the real wave and the river current.

## Authoring a terrain

1. **Make a terrain material.** Open a material, Details > Terrain layers > *Make terrain material*, or write `TERRAIN` / `LAYER` records by hand (`docs/formats/FORMAT_SPECS.md` section 7.x). Per layer: base colour, normal, ORM (occlusion/roughness/metallic) and an optional height map; `tiling` (cm per tile); `surface`, `friction`, `restitution`; an optional foliage type. Tiles are resampled to 256, 512 or 1024 texels (default 512) into an atlas shared by every section using the material.
2. **Pick it.** Landscape mode > Paint tab > Terrain material. The choice is written into the level's `LANDSCAPE` record.
3. **Paint.** Select a layer, choose Paint / Erase / Smooth, set radius, strength, falloff and opacity (Paint stops at the opacity, so a layer can be laid down thin) and drag on the terrain. Fill Layer, Auto-fill By Slope / Height and Remove Unused Layers work on the whole section. Each drag is one undo step. **Save Terrain** writes the paint into the `.ocland`.
4. **Scatter foliage from a layer.** If a layer names a `.ocfoliage` type and a density, *Scatter Foliage From Layer* places it where that layer's paint is heavy enough: deterministic, one undo step, a second run adds more. It is a bake: painting more later does not move foliage already placed.

Paint is disabled while a Play session or Simulate runs (the contact friction reads the weights on physics worker threads and assumes they do not change).

## Rivers and lakes

Landscape mode > **Water** tab. *New River* / *New Lake*, then click on the terrain to place control points: a river runs downstream along its points (a new point is no higher than the previous one unless Shift is held); a lake's points form its outline. Drag a handle to move it (Shift: vertical), Ctrl+click a segment to insert, Alt+click or Del to remove. Per body: material, depth, bank, bank slow-down, flow scale; per river point: width, flow speed, height. Buttons: **Carve Terrain** (lowers the heights along the river or inside the lake, one undo step, one-shot: moving the water later does not move the bed), Snap Points To Terrain, Reverse Direction, Fit Level To Lowest Point.

The records are `RIVER` / `RIVERPT` / `LAKE` / `LAKEPT` in the `.ocworld` (`FORMAT_SPECS.md` section 11.x), so they save with the level and undo with Ctrl+Z. Bodies are built into plain meshes, so rays, shadows and reflections see them.

**Flow.** A river mesh carries the water velocity (cm/s) in its `uv0`. In a material graph, `VertexFlow` outputs it and `FlowNormal` advects the wave normal along it (two copies half a cycle apart, so varying speed does not shear the pattern); with no flow (a lake) `FlowNormal` is exactly `WaveNormal`. A river material is a copy of the sea material's graph with `FlowNormal` in place of `WaveNormal`. A river without it still looks right, it just does not look like it flows.

**Buoyancy.** `GameWater` installs a physics water sampler: for each dynamic body each fixed step it asks for the highest water surface at the body (the ocean's real Gerstner height at the GPU's own clock, lakes, rivers with their current), so boxes bob with the waves and drift down a river. Bodies wider than 3 m sample their corners too and take the slope. This also fixes the old behaviour where a bounded `WATER` record still held bodies up everywhere: outside the bounds a body now falls.

## Scripting

C# (`Aver.Framework`), main thread, never throws:

```csharp
GroundSample g = Landscape.Sample(x, y);          // Found, Z, Normal, DominantLayer, Surface, SurfaceId, Friction, Restitution
float rock = Landscape.LayerWeight(x, y, "Rock");  // 0..1
int id = Surface.IdOf("Stone");                    // FNV-1a 32 of the name, never 0
WaterSample w = Water.Sample(position);            // Over, Submerged, SurfaceZ, DepthCm, Current, Kind
```

`Landscape.Sample` answers inside the section only; `Water.Sample` includes the ocean's waves. The graph nodes that call these (`AN_LandscapeSample`, `AN_LandscapeLayerId`, `AN_LandscapeLayerWeight`, `AN_LandscapeSurface`, `AN_SurfaceId`, `AN_WaterSample`, `AN_EntityWater`) use `TerrainGraph` in `Aver.Framework`. Paint, carve and spline editing are editor commands, not runtime APIs. The C ABI is `framework_terrain_abi.h` (`docs/ABI.md`, "Terrain and water queries").

## Limits and notes

- No runtime (gameplay-time) painting, no vertex waves on rivers and lakes (their ripple is the material's), no swimming locomotion, no tributaries, no erosion, no non-destructive carving.
- Layers are matched to paint by name. Renaming a painted layer detaches its paint; removing a layer from the material folds its paint into the base. Use *Remove Unused Layers* to drop empty section layers (it clears the paint undo history).
- **Downgrade loses paint**: an older engine build re-saving an `.ocland` drops the unknown `LLYR` / `LWGT` chunks.
- Weight textures are re-uploaded whole (about 4 MB each at 1025 samples), at most every 100 ms while painting; very large sections (2049, 4097) will hitch while painting. Atlas memory is about 36 MB per material at 512 tiles.
- Underwater fog still belongs to the ocean only; a camera in a lake does not get the underwater look.
- The derived `<material>#sec` / `#ring` materials appear in the material list (a `#` in the name).
- **Several sections.** Every `LANDSCAPE` record of a level loads: the first is the primary section (the one the editor sculpts, paints, carves and saves, and the one the procedural ring grid follows); the others are read-only extras, each with its own collision body, its own paint weights (derived `<material>#sec.N` materials, runtime textures `land.w0.N` ...) and the shared terrain material. Height, ground and friction queries answer from whichever section contains the point; ring tiles over an authored section are not built. Drawing is distance-culled: only the 16 nearest sections and tiles inside the frustum and 3 km draw, sharing one 192-draw budget, and one out of view for 300 frames drops its GPU meshes. The editor's brush picks the primary section only.
- Level instances: an instanced `.ocworld` that carries rivers or lakes must have its transform applied to their points; this feature does not do it (flagged to the level-instances feature).

## Tests

`OcLandLayersTest`, `OcMatTerrainTest`, `TextureAtlasTest`, `TerrainLayoutTest`, `LayerPaintTest`, `CarveTest`, `WaterBodyTest`, `WaterBodyRecordTest`, `GerstnerWaveTest` (surface solve), `WaterSamplerTest`, `SurfaceFrictionTest`, `GameLandscapeLayersTest`, `FrameworkTest` (terrain ABI), `AbiEnumTest` (water kinds), `MaterialGraphTest` (flow nodes). Not covered by any automated test: the pictures (atlas seams at tile and section edges, pad bleed, the far flat-average blend, flow scroll, the buoyancy bob against the rendered crest). Those are the headed checks listed in the design contract, on D3D12 and Vulkan.
