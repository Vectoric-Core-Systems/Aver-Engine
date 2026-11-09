# Brittle destruction (Voronoi fracture)

Rigid, pre-fractured destruction, alongside the soft-body dents and tears in [SOFTBODY.md](SOFTBODY.md) (interim decision D1 of [plans/WAVE2_UE.md](plans/WAVE2_UE.md): the two share the `HitEvent` type, not a solver).

| Layer | Where | What |
|---|---|---|
| Fracture core | `Aver.Formats`: `aver/formats/Fracture.hpp`, `src/Fracture.cpp` | `fracture::voronoi(mesh, seeds, options)`: seed patterns (uniform, radial, brick/slab, clustered), the cell of each seed from bisector half-spaces found by a bounded grid neighbour search, each cell clipped against the closed source with `csg::Prepared` (one BSP tree for all cells), inside material and planar UVs on the new faces, contact links with face areas. Fixed seed, same pieces. |
| Connectivity | same header | `makeGraph`, `anchorByPlane`, `components` (connected components over the unbroken links). Not coupled to `softbody/Islands.hpp`. |
| Asset | `aver/formats/OcFrac.hpp` | `.ocfrac`, an AVR1 container (subtype `FRAC`): one shared vertex/index pool, per-piece material ranges, hull points, volume and centroid, links with area and break threshold, anchor flags. `buildOcFrac`, `validateOcFrac`, `write/parse/save/loadOcFrac`. |
| Runtime | `Aver.Destruct` (`AVER_MODULE_DESTRUCT`): `aver/destruct/Destructible.hpp` | `Destructible` over the physics C ABI. Compound mode: a body per connected component (a compound of convex hulls), static when it holds an anchor. A `HitEvent` (or a contact impulse over a threshold) adds strain to the links in reach; past the link's threshold it breaks, a component that falls in two becomes two bodies carrying the parent's motion at their own centre of mass, and the freed ones get a share of the hit. Jointed mode: a body per piece, fixed joints with break forces (`aver_phys_joint_set_break`), so the solver decides. |

Units: a fracture works in the source mesh's units; the runtime scales them to cm with `DestructibleDesc::unitScale`. Link thresholds are in the units of the `HitEvent` magnitude (kg*cm/s) in Compound mode and become newtons (times `jointForcePerThreshold`) in Jointed mode.

Limits that are real: a cell that cuts a non-convex source into disconnected islands stays one piece (its hull is then the hull of all the islands); a piece's physics shape is the convex hull of its vertices; the debris cap and sleeping despawn remove pieces, there is no fade; bodies are created per split (shapes are pooled per piece, bodies are not); hierarchical two-level fracture, the editor tool and the per-break audio/FX hook are not done. Tests: `FractureTest` (`tests/framework_rtmesh`), `DestructTest` (`tests/destruct`).
