# Blend shapes, facial animation and lip-sync

Morph targets (blend shapes) deform a mesh by per-vertex offsets scaled by a weight. Aver stores them in the
`.ocmesh`, drives the weights from animation, scripts, the sequencer and a lip-sync player, and applies them on
the GPU in the same compute pass that skins the mesh. Design record: `docs/plans/wave9/MORPH_TARGETS.md`.

## 1. Data

| What | Where |
|---|---|
| Targets | `MRPH` chunk of `.ocmesh` (header flag `kOcMeshHasMorph`, container `contentVersion` 2 only when targets exist). Sparse: a target stores only the vertices it moves, plus optional normal deltas. Up to 512 targets per mesh. Format: `docs/formats/FORMAT_SPECS.md` 5.10. |
| Weights over time | Float curves in an ordinary `.ocanim`, named `morph.<target>`. A clip with curves and no bone tracks is legal (a curve-only clip), so a face with no rig animates. |
| Expressions | `pose.<name>` curves: a named face pose (smile, a viseme) with its own weight. |
| Face rig | `.ocface` (JSON): poses that combine targets and optional bone deltas. Section 4. |
| Lip-sync | `.ocvis` text tracks of phoneme or viseme events. Section 5. |

Import: glTF morph targets (dense and sparse accessors, normalised values, `extras.targetNames`, default
`mesh.weights`, and `weights` animations in LINEAR / STEP / CUBICSPLINE) and FBX blend shapes (via ufbx) become
targets and `morph.*` curves. Names fall back to `Target_<i>`. Position deltas take the same scale and basis
change as positions; a morph target that moves nothing is dropped. Simplified LODs keep their targets
(`remapMorphTargets`). `AverAssetC verify` prints `"morphTargets":N`; `AverAssetC visbake` bakes a track.

## 2. Weights at runtime: `CMorphTargets`

Add **Morph Targets** to an entity (Add Component in the editor, or any weight call from script, which attaches
it). The mesh must carry targets. Per entity, per frame, `MorphSystem` combines in this order:

1. animation curves (`morph.*` and expanded `pose.*`) from the state machine / montage / clip,
2. declared defaults,
3. lip-sync,
4. sequencer overrides, then script overrides (a later layer wins),
5. the entity's master weight,
6. clamp to [-4, 4], drop weights below 2^-12, keep only targets the mesh declares.

At most 64 targets are active on one mesh at once; the heaviest by weight times maximum displacement win.
Overrides ramp in over `blendSeconds` in value and strength. Flags on the component: `inherit` (take the parent
entity's weights, for hair or teeth meshes that share a face's shapes), `off`, `ignoreanim`.

A mesh with targets and no skin draws through a morph-only dispatch while any weight is non-zero and falls back
to the static mesh after 8 idle frames. Bounds grow by the largest possible displacement, so culling stays right.

## 3. Driving weights

**Animation.** Author `morph.<target>` curves in a clip (Anim editor: right column, MORPH TARGETS, key a slider
at the playhead, or Curves > Add morph curve). Montages, slots and state machines carry the curves like any
other pose data: slot nodes and the default-slot montage overlay them, layered nodes blend them with the bone
weight, additive nodes add their difference from the reference.

**State-machine curve node.** An `.ocasm` pipeline `curve` node sets or adds a pose curve from a constant or a
machine parameter (`docs/ANIM_MONTAGES.md` section 2). This ties an expression to a gameplay parameter with no
script.

**Sequencer.** Property tracks may target `CMorphTargets.morph.<target>` and `CMorphTargets.pose.<name>`
(`docs/SEQUENCER.md`); muting a track releases the override. The Sequence editor's `keyMorph` creates the track.

**Script.** Methods on `Entity` (`Aver.Framework`):

```csharp
npc.SetMorphWeight("jawOpen", 0.6f, blendSeconds: 0.1f);
npc.SetFacePose("smile", 1.0f, 0.2f);
npc.ClearMorphWeights();                 // every override; or ClearMorphWeights("smile")
if (npc.TryGetMorphWeight("jawOpen", out float w)) { ... }
npc.SetFaceRig("Faces/Hero.ocface");     // null = the built-in rig
npc.SetMorphMaster(0.5f);                // scales everything
npc.PlayLipSync("Speech/Hello.ocvis", rate: 1f, startSeconds: 0f, strength: 1f);
npc.StopLipSync(0.1f);
npc.SetLipSyncTime(1.25f);               // follow an audio clock instead of free-running
npc.QueueViseme("AH", startSeconds: 0f, durationSeconds: 0.2f, intensity: 1f);
npc.TryGetLipSyncState(out float time, out float length, out float strength);
```

`FaceShapes` holds the 52 ARKit names as constants and `Visemes` the 15 viseme names. The same operations exist
as nine Animation nodes in the graph editor (Set Morph Weight, Set Face Pose, Clear Morph Weights, Get Morph
Weight, Set Face Rig, Play Lip Sync, Stop Lip Sync, Queue Viseme, Get Lip Sync State). Names cross the C ABI
(`framework_morph_abi.h`) as FNV-1a 64 hashes only.

## 4. Face rigs (`.ocface`)

JSON, `"format": "ocface"`, version 1. A rig lists **poses**; a pose lists target references with weights and
optional **joint** deltas (bone-local position, rotation quaternion, scale):

```json
{"name": "talkOpen", "targets": [{"name": "jawOpen", "weight": 0.5}],
 "joints": [{"bone": "jaw", "position": [0,0,0], "rotation": [0.1,0,0,0.99], "scale": [1,1,1]}]}
```

Resolving a pose name `p` at weight `w`, first match wins: (1) an explicit pose in the entity's rig; (2) a mesh
target of the same name; (3) the built-in pose (the 52 ARKit names as identity poses and 15 `viseme_*`
compositions). Joint deltas apply as `rotation = rotation * slerp(identity, q, w)`, `position += p * w`,
scale lerped toward the delta. `phonemes` remaps a phoneme symbol to a viseme pose; `lipsync` holds the
envelope parameters. Limits: 256 poses, 32 targets and 16 joints per pose, names up to 128 bytes. The Face Rig
editor (New > Face Rig; open an `.ocface`) previews poses on a mesh, edits and validates them, and bakes a
viseme track.

## 5. Lip-sync (`.ocvis`)

```
OCVIS 1
SET phoneme
EVENT 0.10 0.22 HH
EVENT 0.22 0.38 AH 0.9
```

`SET phoneme` (ARPAbet, 39 symbols plus `sil`, `sp`, `pau`) or `SET viseme` (`viseme_aa` or `aa`). Events are
`start end symbol [intensity]`, unsorted is fine, `#` comments. The player evaluates one envelope per event
(rise over `lead + attack`, hold at least `minHold`, fall over `release`; smoothstep), takes the largest weight
per viseme and scales the sum down to `maxTotal`. There is no speech recognition: produce `.ocvis` from a
transcript tool or by hand. `AverAssetC visbake track.ocvis --rig x.ocface --out clip.ocanim [--fps 30]` runs the same function
into a curve-only clip, so speech can also play from a montage or sequencer animation track.

## 6. Sample assets

`content/dev/Face.gltf` (24 x 24 head patch, 16 shapes, one sparse), `FaceRigged.gltf` (the same with a two-bone
jaw), `Face.ocface`, `Hello.ocvis`. Regenerate the first three with `build/bin/MakeFace.exe content/dev`; it
re-imports them and checks the numbers.

## 7. Tests

`MorphFormatTest`, `MorphGltfTest` (`tests/formats`); `PoseCurveTest`, `MorphTest`, `FaceRigTest`, `VisemeTest`
(`tests/anim`); `MorphLayoutTest` (`tests/render.skin`); `MorphSystemTest` (`tests/anim.scene`);
`FrameworkMorphAbiTest` (`tests/framework_morph`); `MorphNodeTests` (`Aver.Graph.Tests`); two morph cases in
the skinning self-test.
