# Code notes: softbody

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/softbody/include/aver/softbody/SoftAsset.hpp

- file header: ".ocsoft is JSON with "format":"ocsoft", "version":1, and the paint data is in it."; "Pure text and arithmetic: no scene, no GPU, no Jolt."
- `SoftKind` Jelly and Rope: "wave 11, docs/plans/wave11/SOFTBODY_HAIR.md"
- `ClothSolver::Cage`: the wave 9 path, beams and dihedral bends from the mesh (dents, tears).
- `ClothSolver::Fast`: Jolt makes its own edges, shear edges, bends and long range attachments; the cheap path for flags, curtains and capes; no dents, no tears, no RenderBinding splits.
- `ClothParams`: backstopCm "< 0 off" kept; the "wave 11 (written only when they differ)" marker kept in merged form.
- `InflateParams`: "A closed cloth with a pressure is an inflatable (a beach ball, a balloon)."; pressureKPa "0 = off"; leakKPaPerS: the system ramps pressure down by this much per second (a puncture).
- `GroupParams`: a cloth that never collides with the ragdoll of the entity that owns its skeleton (a cape on a ragdolled character); JSON form "group": { "ragdollOf": "owner" }.
- `JellyParams`: a voxel tetrahedral lattice inside the mesh, render mesh embedded by barycentric coordinates (kept); snapCm: lattice nodes this close to the surface move onto it; volumeCompliance "per tetrahedron, 0 = incompressible"; vertexRadiusCm "< 0: 0.35 x the cell".
- `jellyVertexRadiusCm`: the asset's radius, else 0.35 of the cell, which hides the half-cell the render mesh may poke out.
- `RopeParams`: a chain of Cosserat rods drawn as a generated tube (kept); slack: rest length = distance between the ends x slack, unless the component names a length (kept); material: a material NAME for the tube, empty keeps the entity's (kept).
- `DestructParams`: impactRadiusScale "multiplies the impact deposit radius"; bake: a debris piece asleep long enough is redrawn as a plain static mesh (kept).
- `CapMode`: Auto "on for a destructible, off for cloth" (kept).
- `CapParams`: thicknessCm "< 0: the preset's capThicknessCm"; lodCm "beyond this distance from the camera a piece draws without caps" (kept); interiorMaterial "a material NAME for the walls; empty merges them into the surface mesh".
- `FxSlot` section header: "all written only when they differ from these defaults".
- `FxSlotParams`: particle "content-relative path of a GPU_PARTICLES effect; empty = none" (kept); sounds "variant is hash(seq) % n" (kept); minSpeedCmS "Impact only"; count "base particle count"; countPerMs "plus this per m/s of the event's value (closing speed, or the broken-edge count)"; countPerParticle "Dust: particles = this x the cage particles that became dust" (kept); cooldownS "per entry and slot".
- `ProxyParams`: maxParticles "0 = sim.maxParticles".
- `PaintRule`: the rule pins or limits particles; positions are MESH-LOCAL (the .ocmesh's own space) (kept as "mesh-local"); axis "Above: 0 x, 1 y, 2 z"; valueCm "Above: pinned when the coordinate is >= this"; bone "Bone: pinned when the vertex weight to this bone >= minWeight"; centreCm "Sphere"; texture "Mask: R below pinBelow pins, G scales the reach" (kept).
- `PaintData`: "keyed by meshKey (simMeshKey)"; reach "linear in (0, 2 * defaultReachCm]" (kept); stale(): "True when painted data exists for a different mesh than currentKey."
- `SoftAsset`: "wave 11" section markers.
- `makeSoftAsset`: "material, iterations and damping from the preset table".
- `capsEnabled`/`capThicknessCm`: "Whether this asset's breaks get caps (Auto: a destructible), and their thickness in cm (the preset's when unset)."
- `parseSoftAsset`: "False with why on malformed or invalid input."
- `encodePaintRle`: "Paint byte arrays as hex run-length pairs (value, count), four hex digits per run."
