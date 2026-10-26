# BigWorldLab

Engine test content for the large-world work (`docs/plans/wave11/LARGE_WORLD.md`): one road along +-100 km of X, about 400,000 placements of
the built-in cube and nothing else, laid out to break a chunked, origin-shifted world in the places such a world breaks. It is the content the
100 km flight needs (NeonDistrict is 16 km square and sits inside region 0, so it never rebases), and the scale test of the chunk cook.

The level is **generated**, not committed (about 36 MB of text); the script is the source.

```
python scripts/gen-bigworld.py                        # test-content/BigWorldLab/Content/Maps/BigWorldLab.ocworld
python scripts/gen-bigworld.py --km 10 --per-km 500   # a +-10 km slice for a quick look
AverAssetC cook-chunks test-content/BigWorldLab/Content/Maps/BigWorldLab.ocworld --content-dir test-content/BigWorldLab/Content
```

The cook adds `chunks=Binaries/Chunks/BigWorldLab` to the level's STREAM record and writes the set there (also World Settings > Level
Streaming > Cook Chunks in the editor). The game streams the cooked set; the editor keeps streaming the level's own objects, and Play >
Standalone Game re-cooks a stale set first. Generation is deterministic: two runs give identical files, and so do two cooks.

## What is in it

| Where | What | Why |
|---|---|---|
| x = -50 km (the spawn) | A spinning cube (`Anims/Spin.ocanim`, an object clip), a crate with a `CollisionShape` COMPONENT, the `AN_BigBeacon` graph class | An animated placement, a COMPONENT-carrying placement and a **level-global** class placement (never chunk-owned) |
| the whole road | A 100 m slab every 100 m, a lamp post every 20 m on each side (post + head as a child), eight rows of block buildings (body + roof as a child) | 2,000 placements a kilometre; children travel with their root |
| a skyline building every 40th | Half extents past 3,500 cm | Wide on its own: the cook lists it in the index |
| every region seam in range (12 of them) | A 12 m span centred exactly on the seam and three markers a quarter of a centimetre apart across it | Items straddling the seam, and the f64 owner-chunk split exactly on a chunk boundary |
| x = -1600 n for n in 1, 2, 7, 64, 513, 1000, 6000 | Markers exactly on the chunk boundary and 0.25 cm either side, on x and on y | `floor()` of a negative coordinate: the case the first version of every one of these systems got wrong |
| every 5 km | Six crates and a door frame with an animated leaf (`Anims/Door.ocanim`) | Colliding statics and an animated kinematic placement. The door swings on its hinge edge: a level record cannot carry a hinge joint, so the world-anchored hinge door of `WorldFlightTest` is built in code |
| x = 25 km | A 500 m bridge with eleven pillars | Wide by 25,000 cm of reach: its owner chunk must stay resident while the deck is near, however far the chunk's own cell is |
| everywhere | An infinite PCG field (`Field`) scattering cubes | The chunk generator at 100 km and beyond |

Every position is on a quarter-centimetre grid, so a chunked restore equals a flat load bit for bit (`AuthoredStreamTest` checks that on a
generated grid; this level is for the headed passes).

## Headed checks (the owner)

`LARGE_WORLD.md` section 6 lists them; with this level:

1. Cook it. The report should show roughly 400,000 placements, a handful of level-global (the class), a few thousand wide (every 40th
   building, the bridge), and a set of the order of 100 MB in about 13 region files (the arithmetic of 2.4.4, not measured). Record the real
   size and time in section 9 of the plan.
2. Fly +X from the spawn at 300 m/s (`--cam-fly`): six rebases, at 9.2, 25.6, 42.0, 58.4, 74.8 and 91.1 km of travel. The cooked set streams
   with zero loads and evictions on a rebase frame; the bridge appears as you approach it from 1 km out, not before its pillars.
3. Park on a seam (the spans are the orange boxes) and wiggle +-1 km: one rebase, none while wiggling.
4. Edit the level in the editor (it opens on the records, not the set): the cooked set is reported stale until Cook Chunks runs again, and the
   game falls back to streaming the level from its objects with one warning.

A project that opens this level on a machine with no `Binaries/Chunks` set streams the old way: slower to fill, identical to look at.
