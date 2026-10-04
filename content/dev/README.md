# content/dev — assets the engine tests itself against

Machine-written. **Regenerate, do not hand-edit.**

## Rig.gltf

The tree's only skinned asset source. Written by `tools/MakeRig.cpp`:

```
build/bin/MakeRig.exe content/dev
```

which also imports back what it just wrote and asserts every property downstream code relies on —
the child-first joint reorder, the metre-to-centimetre scale, that vertices are genuinely *blended*
between two bones rather than rigidly parented to one, and that weights still sum to one after the
`R8G8B8A8_UNORM` round trip through `.ocmesh`.

It is **glTF and not the `.oc*` triple** on purpose. A generator that wrote the engine's own
container would be testing only itself, and would become a second source of truth for the skin
stream's interleave and the weight quantisation — both of which live in `Aver.Formats` and would
drift the first time it changed. Going through the importer exercises the scale, the determinant −1
basis change and the joint reorder, which is what a real asset hits. glTF is also text, so this can
be committed and diffed rather than landing as a blob.

A 100 cm square tube along engine +Z, 52 vertices, two bones (`root`, `bend` at +50 cm), and a
one-second clip that bends 90° at t=0.5 and **returns to rest at t=1.0** — the return is what lets a
check assert that a pose is a function of time rather than an accumulation.

### Cooking it

The `.ocmesh` / `.ocskel` / `.ocanim` triple is **not** committed; it is derived:

```
build/bin/ConvertTool.exe content/dev/Rig.gltf <out-dir> Rig
```

producing `Rig.ocmesh`, `Rig.ocskel` and `Rig_Bend.ocanim`. The clip's `skeletonRef` is set to the
base name, because a clip's skeleton is resolved by file stem beside it.
