# Feature: Morph targets (blend shapes) and facial animation

Design contract for the implementing agent. Approved by the owner 2026-10-17 (wave 9). Nothing here is built. Rules in force: upgrade the existing systems (`.ocmesh`, the glTF importer, `Pose`/`sampleAnimation`, `AnimSystem`, `SkinningPass`/`SkinnedScene`, the Anim editor, Property sequencer tracks, the framework ABI relay pattern); no parallel animation or skinning system; D3D12 and Vulkan parity (one HLSL file, no vendor extensions, no atomics); no new third-party code (ufbx arrives through `FBX_IMPORT.md`, stb_truetype is irrelevant here); lean comments; every runtime surface gets C# bindings AND Aver Node nodes; ctest-registered tests that need no GPU. Format and depth follow `docs/plans/wave5/IK_RETARGETING.md`. Patent note: blend shapes, per-vertex delta application, rank bitmaps and viseme tables are decades-old public art and nothing here copies a vendor method (no UE-style atomic scatter, no vendor SDK), but no live patent search was run for this design (open question 8).

## 0. Decisions in one screen

1. **Sparse targets live in the mesh**: `OcMeshData::morphTargets` (`OcMorphTarget`: name, default weight, ascending vertex list, 3 position deltas per entry, optional 3 normal deltas). New optional `MRPH` chunk (version 1) in `.ocmesh`, header flag `kOcMeshHasMorph = 1u << 11`, container `contentVersion = 2` only when targets exist. A mesh without targets writes byte-identical to today; an old reader skips the chunk and loads the base mesh. The macro `AVER_OCMESH_HAS_MORPH` and the names `OcMorphTarget` / `morphTargets` are exactly what `FBX_IMPORT.md` section 2.7 assumes, so the FBX importer needs no edit.
2. **Weights are named float curves.** `OcCurve` already exists in `.ocanim` (`OcAnim.hpp:171`) with tangents, so a weight track is a curve named `morph.<target>`, and a face-pose weight is `pose.<name>`. No `.ocanim` format change except two name constants and allowing a clip with curves and zero tracks (face-only clips need it). The prefix `morph.` is the one `FBX_IMPORT.md` already emits (`kFbxMorphCurvePrefix` is defined as the shared constant).
3. **`Pose` grows a sparse curve channel** (`Pose::curves`: sorted `{kind, id, value}`), so every existing blend (state-machine crossfade, blend space, montage slot, layered, additive, motion matching) carries weights for free. `restPose` clears it, `sampleAnimation` fills it from `morph.` / `pose.` curves, `blendPose`/`blendPoseMasked`/`addPose` blend it with two stated rules (crossfade fades missing curves, overlay leaves them). Seven `.local.swap(` sites that would silently drop it are listed in section 2.4.
4. **One runtime owner: `MorphSystem`** inside `Aver.Anim.Scene`, owned by `AnimSystem` like `MontageSystem`. It turns pose curves + script/sequencer overrides + lip-sync + face-rig poses into one **sparse final weight list per entity** (`MorphFrame`, fnv1a64 of the target name -> weight) and applies face-rig **joint poses** to `Pose::local` just before the pose modifier. It never sees a mesh; the renderer declares each entity's target names back to it.
5. **GPU: the existing skinning compute pass, extended.** Morph is applied to the rest vertex before skinning, in `skin.hlsl`'s `CSSkin`, for skinned and unskinned meshes. Static per-mesh data is the file's own sparse delta lists plus a per-target **rank bitmap** (membership bit + prefix count) so a vertex finds its delta in O(1) with no inverse index and no atomics. Per instance, the CPU uploads a **sparse active list** (at most 64 targets, `{rankBase, deltaBase, weight}`); cost is vertices x active targets, independent of how many targets the mesh has. Zero active targets compiles to the old path (flag bit).
6. **Unskinned morphing meshes use the same machinery**: `SkinnedScene` also walks the new `CMorphTargets` pool; a morph-only resident has no bone ring work, dispatches only when weights changed, and `drawHandle` falls back to the static mesh after the weights have been all zero for 8 frames (so idle faces stay on the cheap static BLAS).
7. **Ray tracing needs no new code**: a posed mesh is already a compute-written mesh, which Voxi refits per frame (`VoxiRenderer.cpp:1811`, `:1829`). Bounds get an exact conservative pad from per-target delta extents. LOD: a morphing entity draws LOD 0 only (same as a skinned entity today); coarser LODs share LOD 0's vertex ids (`OcMesh.hpp:230-243`), so deltas already index them.
8. **Face rig = `.ocface` (JSON) + built-in ARKit-52 defaults.** Poses map names to morph targets and/or joint deltas. Pose resolution: explicit rig pose, else a mesh target of the same name (so an ARKit or Ready-Player-Me style mesh works with no asset at all), else the built-in default table. 15 Oculus-set visemes (`viseme_*`) are built in as ARKit compositions.
9. **Lip-sync = timed events -> viseme pose weights**, one pure function shared by runtime and baker. Authored tracks are `.ocvis` (text); the runtime player also takes events queued from script (TTS results). `bakeVisemeTrack` turns a track into an ordinary curve-only `.ocanim`, so montages, state machines and sequencer animation tracks can play speech with no new playback code. No speech recognition.
10. **Sequencer**: Property tracks gain two virtual fields, `CMorphTargets.morph.<target>` and `CMorphTargets.pose.<name>`. No `.ocworld` format change, no new track kind.
11. **ABI/script**: new `framework_morph_abi.h` (own version, provider relay, the `framework_ik_abi.h` shape). C# `Entity` methods, `FaceShapes`/`Visemes` name constants, nine Aver Node nodes via the generic table (`GraphMorphNodeTable.cs` + `GraphNodeDefsMorph.hpp`).
12. **Editor**: `SkinPreview` (the Anim editor's private skin feature, extracted and made morph-capable), a `MorphPanel` widget hosted in the Anim editor (skeleton and clip tabs) and in a new `.ocface` tab, `CMorphTargets` in Add Component and Details, a generator `tools/MakeFace.cpp` for headed tests.

Size XL. Seven phases, each shippable on its own (section 4).

## 1. What exists today

| Piece | Where | State |
|---|---|---|
| glTF importer skips morph targets | `modules/formats/src/GltfImport.cpp:320` `note("morph targets")` | Primitive `targets` never read. |
| glTF `weights` animation channels dropped | `GltfImport.cpp:1000` (skeletal clips), `:1208` (object clips) | `note("morph-target animation")`. |
| Sparse accessors refused | `GltfImport.cpp:224` | Morph exporters use sparse accessors routinely. |
| Skeletal clips need a skeleton | `GltfImport.cpp:973` `if (r_.skeletons.empty()) return true;`; clip dropped when it has no tracks `:1058` | A face with morph weights and no rig yields no clip. |
| Node basis for normals | `GltfImport.cpp:353-364` (`nrmXf`, rescaled to largest entry 1), applied `:388` | Right for unit normals (renormalised); WRONG scale for normal deltas (section 2.3). |
| One OcMeshData per glTF mesh, primitives appended, one copy per extra basis | `GltfImport.cpp:316` importPrimitive, `:502` importMesh, `:1805`, `:1842` | Deltas must be added per primitive at `baseVertex`. |
| `.ocmesh` data model: no morph field; skin streams; LODs share LOD 0 vertices | `OcMesh.hpp:204-287`, `:230-243`, flags `:13-19` | Bits 0-3, 5, 9 used here; the spec also reserves 4, 6-8, 10 (`FORMAT_SPECS.md:187`), so bit 11 is free. |
| Writer/reader: chunks MHDR STRT VTXS IDXS MADR [MLET]; unknown chunks ignored; `contentVersion = 1` | `OcMesh.cpp:738-750` (write), `:755` (parse), `:1271` (MADR read); `Avr1.hpp:73-90` (`Required` flag is not enforced) | Adding an optional chunk is the established backward-compat route (MLET did it). |
| Mesh ops that renumber vertices | `ClusterBuilder.cpp:1500` `simplifyMesh`, remap `:1628-1648` (joints/weights remapped by hand); `AverAssetC.cpp:767` `mergeAll`, `:697` simplify call | Both would corrupt target indices. |
| Named scalar curves with tangents | `OcAnim.hpp:171` `OcCurve`, `:199` `OcAnimation`, `curves :234`, `curveById :240`; `sampleCurve` `AnimSampler.hpp:41`, `AnimSampler.cpp:159` | The carrier. `OcAnimation::valid()` (`OcAnim.cpp:296`) and `writeOcAnim` (`:307`) REFUSE zero tracks. |
| `Pose` = `std::vector<Transform> local` only; `restPose`, `blendPose`, `blendPoseMasked`, `addPose` | `Pose.hpp:17`, `:26`, `:53`, `:57`, `:77`; `Pose.cpp:15`, `:64`, `:75`, `:118` | No curve channel. |
| `.local.swap(` sites that move only bones | `AnimPipeline.cpp:165,185,198,205`; `Montage.cpp:423`; `AnimMontages.cpp:409`; `AnimSystem.cpp:477` | Seven traps once `Pose` has a second vector. |
| Every blend funnels through the four functions | state machine `AnimStateMachine.cpp:426-451`, blend space `BlendSpace.cpp:388-392`, pipeline `AnimPipeline.cpp:164-205`, montage `Montage.cpp:398-426`, `AnimPlayer` `AnimSampler.cpp:279-288`, `AnimSystem.cpp:436-477` | One edit point. |
| `AnimSystem::tick`: per CAnimator entity; skips entities with no `CSkeletalMesh` rig (`:404`); sample `:436`; montage default-slot blend `:476-477`; pose modifier seam `:533`; `poseToSkinning` after | `AnimSystem.cpp:223-540` | Entities without an animator or rig never get a Posed entry. |
| `AnimSystem` owns subsystems as members (`MontageSystem montages_`, `RetargetRuntime retarget_`), `curveValue` reads the clip's curves by hash | `AnimSystem.hpp:417`, `:427`, `:227`; `AnimSystem::clear` `AnimSystem.cpp:178` | The ownership model for `MorphSystem`. |
| Dynamic component registration through `AnimSystem::registerComponents` (called by both hosts) | `AnimSystem.cpp:91` (`CRootMotion`); `GameSystemsWiring.hpp:125` | `CMorphTargets` registers here: zero host edits. |
| Per-tick reset hook for transient runtime state | `GameSystemsWiring.hpp:177-185` (`resetGameSystems`, `controlRigSystem().clearRuntime()`) | Morph overrides are transient too. |
| Skinning compute pass: `rest`/`bind`/`bones` SRVs, `out` UAV, `b3` root constants (4 dwords), per-instance bone ring + binding sets | `skin.hlsl:1-61`; `SkinningPass.cpp:36-62` (pipeline: `srvCount 3`, `constantDwords[3] = 4`), `:74-184` createMesh, `:199-239` dispatch; `SkinningPass.hpp:47-70` | No morph. Tier-1 rule: every declared SRV slot must hold a valid descriptor (`RHIResources.hpp:331`), so unused slots need a dummy. Max 25 SRVs (`:223`). |
| Skinned residency per entity; walks ONLY the `CSkeletalMesh` pool; returns nullptr when the rig size is unknown | `SkinnedScene.cpp:60-141` acquire, `:143-224` update (pool `:167`), `:226` drawHandle, `:234` prePass, `:243` overlayPass; bounds `:186-203` | An unskinned morphing mesh is invisible to it. |
| Draw path substitutes the posed handle for every pass and re-cuts material parts over it | `GameRender.cpp:663`, `:932`; `GameContent.cpp:740-790`; `SandboxRender.cpp:2396` | Morph-only entities get it for free through `drawHandle`. |
| Compute-written meshes refit their BLAS per frame (updatable BLAS, `kDynamicBlasRefitsPerRebuild`) | `VoxiRenderer.cpp:1811-1845`; `RHI.hpp:529` createSkinTargetMesh, `:544` meshVertexBuffer | RT interplay is automatic. |
| Posed bounds from per-bone rest boxes, written to `CMeshRenderer` | `Pose.hpp:112-131` (`boneRestBounds`, `posedBounds`), `Pose.cpp:230`; `SkinnedScene.cpp:131`, `:190` | Needs a morph pad. |
| GPU skin parity self-test (`--skin-test`): 3 synthetic rigs vs `anim::skinVertices` | `SkinSelfTest.cpp:1-237`; hook `SandboxApp.cpp:1436` | Extend with morph cases. |
| Pipeline node kinds `Slot, Layered, Additive`; `.ocasm` JSON reader rejects unknown node types | `AnimPipeline.hpp:19`; `AnimGraphAsset.cpp:367-373`; validation `AnimStateMachine.cpp:76-96` | A `curve` node kind is additive but an old build refuses the file. |
| Montage/`driveClip` require a skeleton | `AnimMontages.hpp:49-52`; `SequenceTracks.cpp:578-585` | Face-only entities cannot play montages. |
| Sequencer Property tracks write registered component fields through a base store; `CLocal.` special-case; release hooks | `SequenceTracks.cpp:72`, `:483-519`, `:392`, `:757`; `ownerOf` `LevelSequence.hpp:265` | Virtual field is a ~25-line branch. |
| Placement components: `kPlacedKinds`, `kPlacedComponentTypes`, enum + typeId + apply + capture | `PlacedComponentModel.hpp:41-70`; `LevelComponents.hpp:30`; `LevelComponents.cpp:28`, `:145-155`, `:378-392`, `:408-512`; Details `SandboxComponentDetails.cpp:141,279,513-525` | Append one kind last. |
| Provider-relay ABI pattern | `framework_ik_abi.h` (op defines, provider typedef, `aver_fw_set_*_provider`); `IkAbi.cpp`; install `GameApp.cpp:1870`, `SandboxApp.cpp:639`; C# `IkNative.cs`, `Ik.cs`; graph rows `GraphIkNodeTable.cs`, palette `GraphNodeDefsIk.hpp`, includes `GraphNodeDefs.hpp:104-116`, append `:1104-1108`, concat `GraphGameSystemNodeTable.cs:120`; tests `IkNodeTests.cs`, `Program.cs:275-281`; docs `AVER_NODE_NODES.md` | Copy exactly. |
| Anim editor: private `AnimSkinFeature` drives `SkinningPass` for the preview; `buildPreview`; `drawCurves` | `AnimEditor.cpp:185-341`, `:1034`, `:1274`; factory registration `SandboxApp.cpp:468` | The preview only skins meshes with skin streams. |
| Generator precedent for a committed synthetic asset | `tools/MakeRig.cpp`, registered `tests/formats/CMakeLists.txt:252-263`, output `content/dev/Rig.gltf` | `MakeFace` follows it. |
| `[waveN/feature begin]` markers in shared CMake files | `modules/formats/CMakeLists.txt` (`[wave5/sequencer begin]`), `tests/anim.scene/CMakeLists.txt` | Use `[wave9/morph begin/end]`. |

Missing (this feature): everything with morph/blend-shape/face/viseme in its name, tests included (`grep -ri morph` over `tests/ tools/ sandbox/ Runtime/ scripting/` finds nothing).

## 2. Design

### 2.1 Frame order and data flow (contract)

```
host frame (unchanged): tickGameplay (C#: SetMorphWeight / PlayLipSync ...), sequence Pre (Property tracks -> MorphSystem sequencer layer),
  tickAnimGraphs, ragdoll.tick, AnimSystem::tick(dt):
    per animator entity with a rig:
      restPose (clears Pose::curves) -> source/clip/montage/pipeline (curves ride the Pose)
      >>> MorphSystem::evaluate(world, e, skel, pose, dt)      NEW, one call, just before the pose modifier
            reads pose.curves; resolves layers; applies face-rig JOINT poses to pose.local; stores MorphFrame
      poseMod_ (control rig / IK / ragdoll) ; poseToSkinning
    per animator entity WITHOUT a rig that has CMorphTargets: curve-only sample of its CAnimator clip -> MorphSystem::evaluate(skel = nullptr)
    tail: MorphSystem::tickLoose(world, dt)    CMorphTargets entities with no animator at all (script/lip-sync only)
          MorphSystem::resolveInherit(world)   children with kMorphInheritParent copy their parent's frame
  World::flush
  SkinnedScene::update(world, anim, dev)  reads MorphSystem::frame(e) per resident, maps ids -> target indices, stages the active list,
                                          pads bounds; prePass dispatches; drawHandle substitutes the posed mesh
```
No host file changes order. The only host-visible edits are the provider install (2 lines) and `resetGameSystems` (1 line).

### 2.2 `.ocmesh` morph data

In memory (`OcMesh.hpp`, next to `OcMeshData`):

```cpp
inline constexpr u32 kOcMeshHasMorph = 1u << 11;
inline constexpr u32 kOcMeshMaxMorphTargets = 512;
#define AVER_OCMESH_HAS_MORPH 1
struct OcMorphTarget {
    std::string name;                    // unique within the mesh, non-empty, <= 128 bytes
    f32 defaultWeight = 0.0f;
    std::vector<u32> vertices;           // strictly ascending, < vertexCount() (LOD 0 ids; every LOD shares them)
    std::vector<f32> positionDeltas;     // 3 per entry, engine space, cm
    std::vector<f32> normalDeltas;       // 3 per entry, or empty
    // derived by finalize(), never serialised:
    f32 maxAbs[3] = {0, 0, 0};           // per-axis max |positionDelta| (bounds pad)
    f32 maxLen = 0.0f;                   // max |positionDelta| (active-list importance)
    void finalize();
    bool valid(u32 vertexCount, std::string* why = nullptr) const;
};
// OcMeshData: std::vector<OcMorphTarget> morphTargets; bool hasMorph() const { return !morphTargets.empty(); }
```
`OcMeshData::valid()` (`:278`) additionally requires every target `valid()`. `loadOcMesh/parseOcMesh` call `finalize()`.

On disk, chunk `'MRPH'`, `AvrChunk::version = 1`, chunk flags 0 (optional, not `Required`, so tools that never heard of it are unaffected). Little-endian, offsets chunk-relative, names through the file's existing `STRT` string table (intern before `STRT` is emitted, as the submesh names are):

```
u32 TargetCount            1..512
u32 VertexCount            must equal MHDR LOD 0 VertexCount, else the chunk is dropped with an AVER_WARN and the base mesh still loads
u32 Reserved               0
TargetDesc[TargetCount]    32 B each:
  u32 NameRef              STRT offset
  u32 EntryCount           <= VertexCount
  u64 EntryOffset          chunk-relative, 8-aligned
  f32 DefaultWeight
  u32 Flags                bit0 HasNormalDeltas
  u32 Reserved[2]          0
Entries (at EntryOffset), ascending vertex, stride 16 B or 24 B:
  u32 Vertex; f32 dPos[3];                                  // Flags bit0 clear
  u32 Vertex; f32 dPos[3]; u16 dNrm[3] (IEEE half); u16 pad // Flags bit0 set
```
Readers: a chunk `version > 1` is skipped (mesh loads without targets, one warning). Every offset/count is bounded against the chunk size by subtraction (the checks `OcMesh.cpp:860-905` model), entries must be strictly ascending and `< VertexCount`, floats must be finite, names unique (a duplicate is renamed `name_2`, `_3` and warned). `MHDR` flag bit 11 is set by the writer iff targets exist; the reader trusts the chunk, not the flag (like MLET). Writer: `contentVersion = 2` iff targets exist (nothing branches on `contentVersion` today: `Avr1.cpp:229` stores it and no reader compares it), `minReaderVersion` stays 1. `FORMAT_SPECS.md` gets section 5.10 (`MRPH`).

Helpers (all in new `modules/formats/.../MorphTarget.hpp`, the interface other importers and tools use):

```cpp
inline constexpr const char* kOcCurvePrefixMorph = "morph.";   // == FBX_IMPORT kFbxMorphCurvePrefix
inline constexpr const char* kOcCurvePrefixPose  = "pose.";
std::string morphCurveName(std::string_view target);           // "morph." + target
class MorphTargetBuilder {                                      // sparsify, sort, validate; the only way importers fill morphTargets
public:
    explicit MorphTargetBuilder(u32 vertexCount);
    u32  addTarget(std::string name, f32 defaultWeight = 0);   // returns the index; a repeated name is made unique
    void add(u32 target, u32 vertex, Vec3 dPos, const Vec3* dNrm = nullptr);          // adds when called twice for one vertex
    void setSourceMap(std::vector<u32> outVertexToSource);     // FBX: output vertex -> control point
    void addForSource(u32 target, u32 sourceVertex, Vec3 dPos, const Vec3* dNrm);      // expands to every output vertex of that source vertex
    usize finish(OcMeshData& mesh, f32 epsilonCm = 1e-4f);    // drops |d| < eps entries and empty targets (returns how many), sorts, finalizes, sets mesh.morphTargets
};
void remapMorphTargets(OcMeshData& mesh, const std::vector<u32>& oldToNew, u32 newCount);  // ~0u = vertex removed; re-sorts entries (simplifyMesh)
void appendMorphTargets(OcMeshData& dst, const OcMeshData& src, u32 dstVertexBase);        // merge: same-name targets union, others padded (mergeAll)
OcCurve& addMorphCurve(OcAnimation& clip, std::string_view target, OcInterp interp, const std::vector<f32>& times,
                       const std::vector<f32>& values, const std::vector<f32>* inTan = nullptr, const std::vector<f32>* outTan = nullptr);
```
Why an FBX interface at this level: `FBX_IMPORT.md` 2.7 builds `FbxBlendShape` (engine-space sparse deltas keyed by LOD-0 vertex, plus control-point expansion) and attaches them under `#if defined(AVER_OCMESH_HAS_MORPH)`. Its `attachBlendShapes` becomes `MorphTargetBuilder` calls (`setSourceMap` carries "every output vertex whose control point matches"); `DeformPercent / 100` curves go through `addMorphCurve`. This feature also provides the remap its item 4 waits for (`remapMorphTargets` in `simplifyMesh`, `appendMorphTargets` in `mergeAll`), so FBX may drop its "never merge / never LOD a morphing mesh" rule once phase 1 lands. In-between shapes stay out of v1.

Weights range: runtime clamp `[-4, 4]` (`kMorphWeightMin/Max`). Active threshold `kMorphActiveEps = 2.4e-4` (2^-12). Limits: 512 targets per mesh (above: import keeps the first 512 and notes the rest), 256 pose curves per `Pose`.

### 2.3 Import: glTF

All in `GltfImport.cpp`; `GltfImport.hpp`'s `GltfImportResult` is unchanged (morph rides in `meshes[i].morphTargets` and `animations[i].curves`).

- **Sparse accessors** (replaces the refusal at `:224`): per glTF 2.0, start from the base accessor (zeros when it has no `bufferView`), then apply `sparse.count` pairs: `indices` (u8/u16/u32 view, strictly increasing, `< count`) and `values` (same component type and `normalized` as the accessor). Every index and view range is bounds-checked by subtraction like `viewBytes` (`:194-218`); a bad sparse block fails that accessor with a `why`.
- **Targets** (replaces `:320`): per primitive read `targets[i]` `POSITION` (required for a delta; absent = zero), `NORMAL` (optional), `TANGENT` (noted, dropped: the engine vertex has no tangent). Names: `mesh.extras.targetNames[i]`, else `primitive.extras.targetNames[i]`, else `Target_<i>`. Default weights: `mesh.weights[i]` (node-level `weights` noted). The target count of a mesh is the maximum over its primitives; a primitive with fewer simply contributes nothing to the rest. Deltas are added at the primitive's `baseVertex` (`:371`) through a `MorphTargetBuilder` that lives for the duration of `importMesh` and `finish()`es into the `OcMeshData` at its end (so a mesh copy made for another basis, `:1805`, gets its own correctly rotated deltas).
- **Delta transform**: position delta `d` -> `xform(node, d, point=false)` -> `toEngine(.., isDirection=false)` (axes + x100, no translation). **Normal delta `n'` is NOT `nrmXf * dn`**: `nrmXf` is rescaled to a largest entry of 1 (`:360-364`), which is harmless for unit normals (they are renormalised) and wrong for deltas. With `m = nrmXf * n` (the base normal through the same matrix, `L = |m|`), the engine delta is `toEngine(nrmXf * dn, true) / L`, so that `normalize(baseEngine + w * delta)` equals `normalize(nrmXf * (n + w * dn))`. Skipped when the primitive has no NORMAL attribute (flat normals were generated) or `L` is tiny.
- **Weights animation** (replaces the notes at `:1000` and `:1208`): a helper `collectWeightCurves(anim, curves, duration)` runs for EVERY glTF animation before the skeleton early-return at `:973` is reached. A `weights` channel targets a node; `nodes[n].mesh` gives the glTF mesh whose target names (saved at mesh import in `meshTargetNames_`) name the curves. Sampler output is `keys x T` scalars (`x 3` for CUBICSPLINE: in-tangent, value, out-tangent per key, per target; normalized ints are decoded by `readAccessor`); each target becomes `OcCurve{morph.<name>, interp, times, values}` with `inTangents/outTangents` for CUBICSPLINE (tangents are slopes, the same convention the bone-track import uses). Two nodes animating the same target name (ARKit exports repeat head/teeth/tongue) keep the first curve and note a mismatch only if the values differ. Curves are appended to the animation's skeletal clip when one is built; an animation whose only content is weights becomes a **curve-only clip** (`tracks` empty, `skeletonRef` empty when there is no skeleton). `animationNames` stays parallel.
- **Curve-only clips are legal**: `OcAnimation::valid()` (`OcAnim.cpp:297`) and `writeOcAnim` (`:308`) change `tracks.empty()` to `tracks.empty() && curves.empty()`; the reader already tolerates an empty TRKS (verify with the test). Everything that walks `tracks` already tolerates zero (`sampleAnimation` loops).
- **`unsupported`** loses "morph targets" / "morph-target animation" and gains "morph tangents", "node-level morph weights" when present.
- Not imported: sparse accessor on indices (already allowed by the spec only for attributes), `KHR_materials_variants`, tangent deltas.

### 2.4 Animation: weight curves in clips, in poses, in the graph

**Names** (`OcAnim.hpp`): `kOcCurvePrefixMorph = "morph."` (drives mesh target `<rest>`), `kOcCurvePrefixPose = "pose."` (drives face-rig pose `<rest>`). A curve with neither prefix is an ordinary curve and is untouched. `OcCurve` gains `mutable u64 poseId = 0` (NOT serialised): `fnv1a64` of the bare name, computed lazily by the sampler (idempotent write, so concurrent samplers are benign) or by `parseOcAnim`.

**Pose channel** (`Pose.hpp`):

```cpp
struct PoseCurve { u64 id; f32 value; u32 kind; };           // kind: kPoseCurveMorph = 0, kPoseCurveFace = 1; id = fnv1a64(bare name)
struct Pose { std::vector<Transform> local; std::vector<PoseCurve> curves;  /* sorted by (kind, id) */  void swap(Pose&); ... };
inline constexpr u32 kMaxPoseCurves = 256;
void setPoseCurve(Pose&, u32 kind, u64 id, f32 value);        // insert or overwrite; beyond the cap: dropped, one AVER_WARN
const PoseCurve* findPoseCurve(const Pose&, u32 kind, u64 id);
```
- `restPose` clears `curves` (it is the seed every sampler uses, so stale weights cannot leak between frames). Poses of different skeletons never mix curves wrongly: the id is a name.
- `sampleAnimation` (`AnimSampler.cpp:151`) appends/overwrites a `PoseCurve` for each prefixed curve at `seconds` through the public `sampleClipCurves(clip, seconds, pose)`, which the skeleton-less path also calls.
- **Blend rules** (the contract the tests pin):
  - `blendPose(a, b, t, out, CurveBlend mode = CurveBlend::Crossfade)`. **Crossfade**: union of ids; a curve missing on one side reads 0 there (a state without face curves fades the face out). **Overlay**: a curve only in `a` is kept, only in `b` becomes `b * t`, in both lerps. `Pose.cpp:64-73` copies `curves` before the size check (a size mismatch returns `a` whole, as bones do).
  - Overlay is passed at: `AnimPipeline.cpp:164` and `:197` (slot montage over the running pose), `AnimSystem.cpp:476` (default-slot montage). Crossfade (the default) everywhere else, including `AnimSystem.cpp:442` and every call in the state machine, blend space and `AnimPlayer`.
  - `blendPoseMasked`: overlay semantics with curve weight `s = max_i clamp01(w[i])` (a head-only layer fully applies its face curves; a layer whose clip has no curves leaves the base's face alone).
  - `addPose(base, additive, additiveRest, w, out)`: for each curve in `additive`, `out = base_or_0 + (additive - rest_or_0) * w`.
- **The seven `.local.swap(` sites** (section 1) become `a.swap(b)`; `Pose::swap` swaps both vectors. This is the one mechanical trap: leaving any of them silently drops weights after a slot or layered node.
- Not touched on purpose: `Inertializer` (bones only; curves crossfade through the state machine), `PoseDatabase`/motion matching feature extraction, retargeting (it already copies `curves` by name: `Retarget.cpp:470`, so `morph.`/`pose.` curves survive a retarget bake).
- **Graph (`.ocasm`) curve node**: `PipeNodeKind::Curve = 3`, JSON `{"type":"curve","curve":"morph.browInnerUp"|"pose.smile","valueParam":"Anger","value":0,"mode":"set"|"add","weight":1,"weightParam":""}`. Evaluate: `v = (valueParam ? param : value)`, `w = clamp01(weight * weightParam)`; `set`: `curve = lerp(existing_or_0, v, w)`; `add`: `curve += v * w`. It needs no source, so `AnimPipeline::bind/tick` skip it, `AnimStateMachine.cpp:76` validates `curve` non-empty with a prefix, the writer (`AnimGraphAsset.cpp:96-120`) and reader (`:367`) gain the case. An `.ocasm` using it is refused by older builds (`unknown pipeline node type`); documented. This is how a designer drives an expression from a machine parameter with no script.
- **Montage**: no code. A montage segment's clip curves flow through `MontagePlayer::evaluate` (`Montage.cpp:398-426`) and `evaluateSlot`; slots blend with overlay semantics. Limitation (stated, not fixed): montages and `driveClip` need a skeleton, so a skeleton-less face animates through its `CAnimator` clip or sequencer Property tracks.
- **Sequencer**: Property tracks (section 2.5, "Layers").

### 2.5 Runtime: `CMorphTargets` and `MorphSystem`

New `modules/anim.scene/include/aver/anim/MorphSystem.hpp`, `src/MorphSystem.cpp`. Owned by `AnimSystem` (`MorphSystem& AnimSystem::morphs()`, member `morph_`, constructed with `this`), global shortcut `anim::morphSystem()`. Registered by `AnimSystem::registerComponents` (`AnimSystem.cpp:91`, one added call), cleared by `AnimSystem::clear` (`:178`, caches only) and `resetGameSystems` (`clearRuntime`, transient state).

```cpp
struct CMorphTargets {                 // dynamically registered, zero-fill safe, 24 B, padding-free (static_assert)
    u64 faceRig = 0;                   // .ocface ObjectId; 0 = built-in ARKit defaults only
    u32 flags = 0;                     // kMorphOff = 1 (no output), kMorphInheritParent = 2, kMorphIgnoreAnim = 4
    f32 weight = 0.0f;                 // master scale; 0 reads as 1 (the CAnimator.blendWeight convention); use kMorphOff for off
    u32 reserved[2] = {};
};
struct MorphWeight { u64 id; f32 w; };               // id = fnv1a64(target name)
struct MorphFrame { std::vector<MorphWeight> active; // sorted by id, only |w| >= kMorphActiveEps
                    u32 revision = 0; bool moving = false; };
struct MorphDecl { u64 id; f32 defaultWeight; };     // declared by the renderer from the mesh
```
Fields registered with `World::registerComponent<CMorphTargets>("CMorphTargets")` like `IkRigSystem.cpp:42-49` (`faceRig` I64, `flags` I32, `weight` F32, reserved read-only), `b.verify(sizeof)`.

API (all main-thread):

```cpp
static u32 registerComponent(scene::World&);   static CMorphTargets* attach(World&, Entity, u64 rig = 0, u32 flags = 0);
void evaluate(World&, Entity, const fmt::OcSkeleton* skel, Pose* pose, f32 dt);   // AnimSystem calls it; pose null when skel null
void tickLoose(World&, f32 dt);  void resolveInherit(World&);                      // tail of AnimSystem::tick
const MorphFrame* frame(Entity) const;                                              // the renderer's read side
void declareTargets(Entity, std::span<const MorphDecl>);                           // SkinnedScene on acquire; empty span on retire
bool setTarget(World&, Entity, u64 id, f32 w, f32 blendSec, Layer = Script, u64 owner = 0);
bool setPose  (World&, Entity, u64 id, f32 w, f32 blendSec, Layer = Script, u64 owner = 0);
bool clear(Entity, u64 id /*0 = all*/, u32 kindMask, f32 blendSec, Layer = Script, u64 owner = 0);
void releaseOwner(u64 owner);                                                      // sequencer track release
bool weightOf(Entity, u64 id, f32& out) const;                                     // the FINAL weight (0 when inactive)
bool setFaceRig(World&, Entity, u64 asset);
bool lipSyncPlay(World&, Entity, u64 track, f32 rate, f32 start, f32 strength);  void lipSyncStop(Entity, f32 blendOut);
bool lipSyncSetTime(Entity, f32 t);  bool lipSyncQueue(Entity, u64 symbol, f32 startFromNow, f32 duration, f32 intensity);
bool lipSyncState(Entity, f32& time, f32& length, f32& strength, bool& playing) const;
void clearRuntime();  void clear();  u32 evaluatedLastTick() const;
```
`enum class Layer : u8 { Sequencer, Script }`.

**Layers, in this order, per entity per tick** (all keyed by id; "final" = after step 7):
1. **Animation**: `pose.curves` (or, with no rig, the `CAnimator` clip sampled curve-only via `sampleClipCurves`) unless `kMorphIgnoreAnim`. Kind 0 adds to target weight `id`. Kind 1 expands through the face rig (below) and adds. With `kMorphInheritParent`, this layer is replaced by the nearest ancestor's final frame (`CHierarchy::parent`, `Components.hpp:43`), applied in `resolveInherit` after every entity evaluated (children may precede parents in pool order).
2. **Defaults**: declared `defaultWeight` is the value for a target no layer drives (glTF initial weights). Declared by the renderer, so on an entity's very first frame it is absent (one frame at neutral; accepted).
3. **Lip-sync**: advances its clock (`time += dt * rate`), evaluates viseme pose weights (2.6), expands through the rig, adds. A queued-events player and a track player coexist (events merge). It ends itself when every event has expired and no track plays; `strength` scales its output; stop blends out linearly.
4. **Sequencer layer** then 5. **Script layer**: overrides `{value, alpha}` keyed `(id, kind)`; `v = lerp(v, value, alpha)`. `setTarget`/`setPose` ramps `value` linearly to the new value over `blendSec` and `alpha` to 1 over `blendSec` (0 = immediate); `clear` ramps `alpha` to 0 then removes. Script overrides persist until cleared (transient across Play/level: `clearRuntime`). Sequencer overrides are owned by a track (`ownerOf`, `LevelSequence.hpp:265`); a track that stops calling is released by `releaseOwner` (see Sequencer below), not by a timeout.
6. **Master**: `* (weight == 0 ? 1 : weight)`; `kMorphOff` zeroes the frame.
7. **Clamp** to `[-4, 4]`, drop `|w| < kMorphActiveEps`, sort by id, set `moving` when any weight moved more than 1e-5 or the active set changed, bump `revision`.

Ids the mesh does not declare are dropped at step 7 once the renderer has declared targets (so `frame()` stays exactly the mesh's active set); before any declaration everything is kept (headless tests).

**Face-rig joint poses** (the one place `evaluate` edits `Pose::local`): after the layers above, every rig pose with a non-zero **pose weight** (pose-kind curves + lip-sync + overrides of kind pose, clamped `[0,1]`) applies its joint deltas to `pose->local` (before the pose modifier, so IK/ragdoll see them). Joint delta record `{bone, position add (cm), rotation delta quat, scale multiplier}` is applied with weight `w`: `position += p * w`, `rotation = (rotation * slerp(identity, q, w)).normalized()`, `scale *= lerp(1, s, w)`. The delta is in the bone's local frame; the test uses a real parented rig with non-identity rest rotations (the `IkTest.testParentedChainWithRestRotations` lesson that `Quat` composition order is easy to invert in this engine, `docs/plans/wave5/IK_RETARGETING.md:21`). Bone names bind to the skeleton once per (rig, skeleton) pair; a bone the skeleton lacks is skipped and warned once.

**Sequencer (virtual Property fields)**: in `SequencePlayer::applyProperty` (`SequenceTracks.cpp:483`), before `w.fieldId(tr.property)`: if `tr.property` starts with `CMorphTargets.morph.` or `CMorphTargets.pose.`, require the actor to carry `CMorphTargets` (a track never adds one; warn once otherwise), sample with `sampleSeqProperty(tr, t, false, false, v)` (arity 1) and call `morphs().setTarget/setPose(e, fnv1a64(rest), v[0], 0, Layer::Sequencer, ownerOf(index))`. `releaseTriggered()` (`:757`) and the mute branch (`:392`) call `morphs().releaseOwner(ownerOf(i))` for such tracks. `movesEntity` (`:72`) is unaffected. Format: `SEQTRACK property bind Hero prop CMorphTargets.morph.jawOpen` + `SEQKEY` lines, byte-compatible with every existing parser (an old build warns "no such field" and ignores the track).

### 2.6 Face rig and lip-sync assets

New in `Aver.Anim` (pure): `FaceRig.hpp/.cpp`, `Viseme.hpp/.cpp`.

**`.ocface`** (JSON, written and parsed like `.ocikrig`: nine significant digits, unknown keys ignored, helpers from `modules/anim/src/AnimJson.hpp`):

```json
{ "format": "ocface", "version": 1, "convention": "arkit52",
  "mesh": "Characters/Hero_Head.ocmesh", "skeleton": "Characters/Hero.ocskel",
  "poses": [
    { "name": "smile", "targets": [ {"name": "mouthSmileLeft", "weight": 0.9}, {"name": "mouthSmileRight", "weight": 0.9} ],
      "joints": [ {"bone": "jaw", "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]} ] } ],
  "phonemes": { "AA": "viseme_aa" },
  "lipsync": { "lead": 0.04, "attack": 0.05, "release": 0.08, "minHold": 0.06, "maxTotal": 1.0 } }
```
`mesh`/`skeleton` are editor-preview hints only. Limits: 256 poses, 32 targets and 16 joints per pose, names <= 128 bytes, weights finite in `[-4,4]`; invalid files fail with a `why`. `convention` is informational (`arkit52`, `custom`).

**Pose resolution** (`FaceRigRuntime::expand(poseId, w, out)`), first match wins: (1) an explicit pose of that name in the entity's rig, (2) a mesh target of that name (identity, weight `w`; ids the mesh declares), (3) the built-in default rig's pose of that name. Rule 2 is what makes ARKit-named and Ready-Player-Me-style meshes work with no `.ocface`; rule 3 supplies the `viseme_*` compositions to ARKit-only meshes, while a mesh that already carries `viseme_*` targets uses them (no double drive).

**Built-in defaults** (`anim::defaultFaceRig()`): the 52 ARKit names as `FaceShapes` (browDownLeft browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight eyeBlinkLeft eyeBlinkRight eyeLookDownLeft eyeLookDownRight eyeLookInLeft eyeLookInRight eyeLookOutLeft eyeLookOutRight eyeLookUpLeft eyeLookUpRight eyeSquintLeft eyeSquintRight eyeWideLeft eyeWideRight jawForward jawLeft jawOpen jawRight mouthClose mouthDimpleLeft mouthDimpleRight mouthFrownLeft mouthFrownRight mouthFunnel mouthLeft mouthLowerDownLeft mouthLowerDownRight mouthPressLeft mouthPressRight mouthPucker mouthRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthSmileLeft mouthSmileRight mouthStretchLeft mouthStretchRight mouthUpperUpLeft mouthUpperUpRight noseSneerLeft noseSneerRight tongueOut) are a naming convention only (no code from any vendor), and 15 viseme poses with starting values (art, tunable; tests assert structure, not these numbers):

| Pose | Targets (weight) |
|---|---|
| viseme_sil | none |
| viseme_PP | mouthClose .8, mouthPressLeft .5, mouthPressRight .5 |
| viseme_FF | mouthRollLower .6, mouthUpperUpLeft .3, mouthUpperUpRight .3, jawOpen .1 |
| viseme_TH | tongueOut .5, jawOpen .15 |
| viseme_DD | jawOpen .2, mouthUpperUpLeft .15, mouthUpperUpRight .15 |
| viseme_kk | jawOpen .25, mouthFunnel .1 |
| viseme_CH | mouthFunnel .5, mouthPucker .3, jawOpen .15 |
| viseme_SS | mouthSmileLeft .3, mouthSmileRight .3, jawOpen .05 |
| viseme_nn | jawOpen .15, mouthClose .1 |
| viseme_RR | mouthPucker .4, mouthFunnel .25, jawOpen .15 |
| viseme_aa | jawOpen .7, mouthLowerDownLeft .3, mouthLowerDownRight .3 |
| viseme_E | jawOpen .3, mouthSmileLeft .4, mouthSmileRight .4, mouthStretchLeft .2, mouthStretchRight .2 |
| viseme_ih | jawOpen .25, mouthSmileLeft .3, mouthSmileRight .3 |
| viseme_oh | jawOpen .4, mouthFunnel .6, mouthPucker .2 |
| viseme_ou | jawOpen .2, mouthPucker .7, mouthFunnel .4 |

and the ARPAbet table (39 symbols) -> viseme: AA aa, AE aa, AH aa, AO oh, AW ou, AY aa, B PP, CH CH, D DD, DH TH, EH E, ER RR, EY E, F FF, G kk, HH ih, IH ih, IY ih, JH CH, K kk, L DD, M PP, N nn, NG kk, OW oh, OY oh, P PP, R RR, S SS, SH CH, T DD, TH TH, UH ou, UW ou, V FF, W ou, Y ih, Z SS, ZH CH; silence symbols `sil`, `sp`, `pau` map to `viseme_sil`. A symbol that is a viseme name (`aa`, `PP`, `viseme_aa`) is accepted as is; an unknown symbol is ignored with one warning per symbol.

**`.ocvis`** (text, same reader style as `.ocstrings`: one record per line, `#` comments, numbers via `fromChars`):

```
OCVIS 1
SET phoneme            # phoneme | viseme: which symbol set EVENT lines use (default phoneme)
EVENT 0.00 0.12 sil
EVENT 0.12 0.31 HH 0.8 # start end symbol [intensity 0..1, default 1]
```
Events need not be sorted (sorted on load); `end > start`; at most 65 536 events. Unknown records are ignored. `FORMAT_SPECS.md` gets sections 11j (`.ocface`) and 11k (`.ocvis`) (letters assigned at merge if taken).

**Lip-sync evaluation** (`evalVisemeWeights(events, params, t, out)` in `Viseme.cpp`; pure; the runtime and the baker both call it): for an event with window `[s, e]`, `e' = max(e, s + minHold)`, `rise = smoothstep((t - (s - lead)) / (lead + attack))`, `fall = 1 - smoothstep((t - e') / release)`, `weight = intensity * rise * fall` (clamped 0..1); per viseme pose id take the max over events; if the sum exceeds `maxTotal` scale all down to `maxTotal`. Output is pose-kind weights (`viseme_*` ids), expanded by the rig like any pose. `bakeVisemeTrack(track, rig, fps = 30) -> OcAnimation` samples that function into `pose.viseme_*` Linear curves (duration = last event end + release; curve-only clip, `flags` 0), optionally dropping keys a linear segment reproduces within 1e-3.

### 2.7 GPU application

**Shader (`skin.hlsl`, one file for D3D12 and Vulkan; same entry `CSSkin`)**. New bindings and constants:

```hlsl
struct MorphDelta  { float3 dp; float3 dn; };                 // 24 B, the same float3-pair idiom as SkinVertex
struct MorphActive { uint rankBase; uint deltaBase; float weight; uint pad; };   // 16 B
StructuredBuffer<uint2>        gMorphRank  : register(t3);    // per target: ceil(V/32) words {membership bits, bits set in earlier words}
StructuredBuffer<MorphDelta>   gMorphDelta : register(t4);    // all targets' entries, file order (ascending vertex per target)
StructuredBuffer<MorphActive>  gMorphActive: register(t5);    // this instance's sparse active list
cbuffer SkinParams : register(b3) { uint gVertexCount; uint gBoneCount; uint gFlags; uint gActiveCount; };   // still 4 dwords
// gFlags bit0 = apply morph, bit1 = no skin (morph-only instance: bones/bind are never read)

float3 p0 = r.pos, n0 = r.nrm;
if (gFlags & 1u) {
    const uint word = v >> 5, bit = 1u << (v & 31);
    for (uint k = 0; k < gActiveCount; ++k) {
        const MorphActive a = gMorphActive[k];
        const uint2 rw = gMorphRank[a.rankBase + word];
        if ((rw.x & bit) == 0) continue;
        const MorphDelta d = gMorphDelta[a.deltaBase + rw.y + countbits(rw.x & (bit - 1u))];
        p0 += a.weight * d.dp;  n0 += a.weight * d.dn;
    }
    const float l = length(n0); if (l > 1e-8f) n0 /= l;
}
// then the existing skinning loop reads p0/n0 instead of r.pos/r.nrm; the "no surviving influence" fallback
// and the (gFlags & 2) morph-only path both write p0/n0; uv is still carried through untouched.
```
The loop count is uniform across the wave, there are no atomics, no wave intrinsics, no 64-bit types, `minShaderModel` stays 60. The delta stride is checked the same way the uv stride is: the self-test compares numbers, so a layout disagreement between C++ and DXC/SPIR-V shows up as a wrong answer, not silence.

**Pipeline** (`SkinningPass::init`, `SkinningPass.cpp:36-62`): `srvCount = 6` (all `StructuredBuffer`), `uavCount = 1`, `constantDwords[3] = 4`. Every binding set binds all six SRVs: a plain skinned mesh binds a 64-byte zero **dummy** buffer (owned by the pass) at t3-t5 with count 1, a morph-only mesh binds the dummy at t1 (bind) and a 1-bone ring at t2. Mirror whatever slot-kind declaration the existing pipeline/binding-set code uses for Vulkan (the existing `--skin-test --backend vulkan` run is the proof).

**Static per-mesh residency** (new `render.skin/.../MorphLayout.hpp/.cpp`, pure, no RHI): 

```cpp
struct MorphLayout { u32 vertexCount, targetCount, wordsPerTarget; std::vector<u32> rank;   // targetCount*wordsPerTarget*2 : {bits, prefix}
                     std::vector<f32> delta;     // 6 floats per entry (dp xyz, dn xyz; dn zero when the target has none)
                     std::vector<u32> deltaBase, entryCount; std::vector<f32> maxLen; };
bool buildMorphLayout(const std::vector<fmt::OcMorphTarget>&, u32 vertexCount, MorphLayout&, std::string* why);   // refuses > 128 MiB total
struct MorphTargetWeight { u32 target; f32 weight; };
u32  buildActiveList(const MorphLayout&, const MorphTargetWeight* in, u32 n, MorphActiveRecord out[kMorphMaxActive]);  // caps at 64 by |w|*maxLen
void evalMorphLayout(const MorphLayout&, const MorphActiveRecord*, u32 n, u32 vertex, Vec3& dp, Vec3& dn);   // C++ twin of the loop, for tests
```
`SkinningPass` keeps a refcounted `MorphGpu {rank, delta buffers, layout meta}` per `morphKey` (SkinnedScene passes the mesh id; the editor passes a path hash), uploaded once. `SkinnedMeshGpu` (`SkinningPass.hpp:47-70`) gains `morphKey`, `activeBuf[kRing]` (Upload, 64 x 16 B, **inside the per-instance ring**: the pass-level ring bug in `SkinningPass.hpp:40-46` must not recur), `activeCount`, `noSkin`. `createMesh(mesh, boneCount, out, existingOut = 0, morphKey = 0)` accepts `boneCount == 0` when `mesh.hasMorph()` (morph-only). `dispatch(ctx, m, skin, boneCount, const MorphTargetWeight* w = nullptr, u32 n = 0)` is the old call plus an optional list (old callers unchanged); `dispatchMorph(ctx, m, w, n)` is the morph-only form. Output state handling is unchanged (`skinTransition` to `GeometryRead`, back to `Common` in `overlayPass`).

**Scene join (`SkinnedScene`)**: `acquire` additionally decodes `morphTargets` (it already re-reads the `.ocmesh` for the skin streams, `:83-102`), builds `id -> target index` once per mesh id (`fnv1a64(name)`), keeps `MorphDecl` and calls `anim.morphs().declareTargets(e, decls)`. `update` (a) for skinned residents reads `morphs().frame(e)` and stages `MorphTargetWeight`s when `revision` changed; (b) walks the `CMorphTargets` pool for entities with no `CSkeletalMesh` (skipping ones already handled) and acquires **morph-only** residents (no bones needed: this is what removes the `bones == 0 -> nullptr` deferral at `:116` for them); (c) pads bounds (below); (d) marks `dirty` when a dispatch is owed. A morph-only resident dispatches only when `dirty` or `moving`; once the active set has been empty for 8 consecutive frames it dispatches one rest write (zero active, `noSkin`) and `drawHandle` returns 0 (static mesh, static BLAS); a non-empty set switches `drawHandle` back to the posed mesh the same frame (the buffer was seeded at rest, `RHI.hpp:529`). `drawHandle` for a skinned resident is unchanged. A morphing mesh whose layout build was refused logs once and draws unmorphed. `kMaxResident = 256` is shared. Accessor for editors: `bool morphTargetNames(Entity, std::vector<std::string>&) const`.

### 2.8 Ray tracing, BLAS refit, bounds, LOD

- **BLAS**: nothing to add. A posed (skin-target) mesh has `meshVertexBuffer != 0`, which Voxi already builds updatable and refits every frame (`VoxiRenderer.cpp:1811`, `:1829-1845`) and which the ray-hit shaders read per mesh (`356eb20e`). Morph-only meshes pay that refit only while they are non-idle (the 8-frame fallback to the static mesh above); both transitions are just a different `MeshHandle` in the draw list, exactly like entering/leaving skinning. A face with 10k vertices refits in well under a millisecond; a crowd of 50 morphing faces is the budget to measure (section 6).
- **Bounds**: `OcMorphTarget::maxAbs` gives `pad = sum over active targets of |w| * maxAbs` per axis. `posedBounds(...)` (`Pose.hpp:129`) gets a trailing `const Vec3& pad = {0,0,0}` that expands every bone's rest box before it is transformed (the deltas are applied in rest space, before skinning, so this is exact-conservative, not a heuristic); unskinned: rest AABB expanded by `pad`. Written to `CMeshRenderer` like today (`SkinnedScene.cpp:190-203`). Culling, picking and the GI box already read it.
- **LOD**: `OcMeshData::coarserLods` index LOD 0's vertex ids (`OcMesh.hpp:230-243`), so every delta is valid for every LOD with no change. v1 still draws a morphing entity at its posed LOD 0 (the host substitutes one posed handle per entity, `GameRender.cpp:663`, and `createMeshSharingVertices` refuses compute-written sources, `RHI.hpp:515`); cooked LOD generation keeps the targets (`remapMorphTargets` in `simplifyMesh`). Per-LOD posed index buffers via `createPosedPartMesh` are the follow-up and need no format change.
- **Not lightmapped / not merged**: a mesh with targets is skipped by the lightmap unwrap callers' vertex duplication the same way skinned meshes are (documented in the importer note), and `mergeAll` uses `appendMorphTargets`.

### 2.9 ABI, C#, graph nodes

**`framework_morph_abi.h`** (new, `AVER_FW_MORPH_ABI_VERSION 1`, `aver_fw_morph_abi_version()`), the `framework_ik_abi.h` shape: `typedef int32_t (AVER_FW_CALL* aver_fw_morph_fn)(int32_t op, int32_t entity, int64_t h0, int64_t h1, int64_t h2, const float* in, int32_t inCount, float* outF, int64_t* outI, void* user);` and `aver_fw_set_morph_provider(fn, user)`. Strings are never passed, only fnv1a64 hashes. Ops:

| Op | Value | Arguments |
|---|---|---|
| `AVER_FW_MORPH_OP_SET_TARGET` | 1 | h0 target hash, in[0] weight, in[1] blend seconds (<0 = 0) |
| `AVER_FW_MORPH_OP_SET_POSE` | 2 | h0 pose hash, in[0], in[1] |
| `AVER_FW_MORPH_OP_CLEAR` | 3 | h0 name hash (0 = all), h1 kind mask (1 targets, 2 poses, 3 both), in[0] blend seconds |
| `AVER_FW_MORPH_OP_GET_WEIGHT` | 4 | h0 target hash; outF[0] final weight; returns 1 when the entity has a morph frame (the id need not be active) |
| `AVER_FW_MORPH_OP_SET_RIG` | 5 | h0 `.ocface` ObjectId (0 = built-in defaults) |
| `AVER_FW_MORPH_OP_LIPSYNC_PLAY` | 6 | h0 `.ocvis` ObjectId, in[0] rate (0 = 1), in[1] start seconds, in[2] strength (<0 = 1) |
| `AVER_FW_MORPH_OP_LIPSYNC_STOP` | 7 | in[0] blend-out seconds (<0 = 0.1) |
| `AVER_FW_MORPH_OP_LIPSYNC_TIME` | 8 | in[0] seconds (audio-slaved clock) |
| `AVER_FW_MORPH_OP_LIPSYNC_QUEUE` | 9 | h0 symbol hash (phoneme or viseme name), in[0] start seconds from now, in[1] duration, in[2] intensity |
| `AVER_FW_MORPH_OP_LIPSYNC_STATE` | 10 | outF[0] time, [1] length (0 = open-ended), [2] strength; returns 1 while playing |
| `AVER_FW_MORPH_OP_INFO` | 11 | outI[0] declared target count, outI[1] rig pose count, outI[2] rig hash; outF[0] master weight; returns 1 when the entity has `CMorphTargets` |

Public functions mirror each op (`aver_fw_morph_set_target(int32_t entity, int64_t target, float weight, float blendSeconds)`, `..._set_pose`, `..._clear(entity, name, kindMask, blendSeconds)`, `..._get_weight(entity, target, float* out)`, `..._set_rig`, `..._lipsync_play(entity, track, rate, start, strength)`, `..._lipsync_stop`, `..._lipsync_set_time`, `..._lipsync_queue(entity, symbol, start, duration, intensity)`, `..._lipsync_state(entity, float* out3)`, `..._info(entity, int64_t* outI3, float* outF1)`); with no provider every call returns 0. The provider `anim::morphProvider(op, entity, h0, h1, h2, in, inCount, outF, outI, user)` lives in `MorphSystem.hpp/.cpp` with the `MorphSystem` as `user`; `SET_TARGET`/`SET_POSE`/`LIPSYNC_*`/`SET_RIG` **auto-attach** `CMorphTargets` when the entity has a `CMeshRenderer` and none yet (as `SetIkRig` attaches `CIkRig`), refusing a dead entity or one with no mesh. Install: `aver_fw_set_morph_provider(&anim::morphProvider, &anim::animSystem().morphs())` beside the IK install in both hosts (`GameApp.cpp:1870`, `SandboxApp.cpp:639`).

**C#** (`scripting/csharp/Aver.Framework/`): `MorphNative.cs` (`internal static class FwMorph`, DllImports, the `IkNative.cs` shape), `Morph.cs` (`public readonly partial struct Entity`, helper `MorphHash`, plus `internal static class MorphGraphInterop`), `FaceShapes.cs` (`public static class FaceShapes` with the 52 ARKit names and `public static class Visemes` with the 15 `viseme_*` names plus `AllShapes`/`All`). Entity surface (exact):

```csharp
public bool SetMorphWeight(string target, float weight, float blendSeconds = 0f);
public bool SetFacePose(string pose, float weight, float blendSeconds = 0f);
public bool ClearMorphWeights(string? name = null, float blendSeconds = 0f);   // null = every script override (targets and poses)
public bool TryGetMorphWeight(string target, out float weight);
public bool SetFaceRig(string? rigAsset);                                       // null clears to the built-in defaults
public bool SetMorphMaster(float weight);                                       // SetFloat("CMorphTargets.weight")
public bool PlayLipSync(string trackAsset, float rate = 1f, float startSeconds = 0f, float strength = 1f);
public bool StopLipSync(float blendSeconds = 0.1f);
public bool SetLipSyncTime(float seconds);                                      // slave the clock to an audio voice's playhead
public bool QueueViseme(string symbol, float startSeconds, float durationSeconds, float intensity = 1f);
public bool TryGetLipSyncState(out float time, out float length, out float strength);
public bool IsLipSyncPlaying { get; }
public int  MorphTargetCount { get; }
```
Weights set from script are transient (not saved). `Assets.ObjectIdOf` is the hash, as in `Ik.cs:25`.

**Nodes** (generic table; the parser needs no `case`, per `GraphGameSystemNodes.AddDefaultPins`): new `Aver.Graph/GraphMorphNodeTable.cs` (`MorphTable()`, owner `MorphGraphInterop`), `.Concat(MorphTable())` added at `GraphGameSystemNodeTable.cs:120`; palette mirror `sandbox/src/GraphNodeDefsMorph.hpp` (`appendMorphNodeDefs`, category "Animation", included at `GraphNodeDefs.hpp:104-116`, called at `:1104-1108`). Rows:

```csharp
Reg("setmorphweight",   GenericKind.Exec, typeof(MorphGraphInterop), "SetMorphWeightForGraph",   "entity:int,weight:float,blendSeconds:float=0", "success:bool", "p:entity,@target,p:weight,p:blendSeconds", "success"),
Reg("setfacepose",      GenericKind.Exec, typeof(MorphGraphInterop), "SetFacePoseForGraph",      "entity:int,weight:float,blendSeconds:float=0", "success:bool", "p:entity,@pose,p:weight,p:blendSeconds", "success"),
Reg("clearmorphweights",GenericKind.Exec, typeof(MorphGraphInterop), "ClearMorphWeightsForGraph","entity:int,blendSeconds:float=0",              "success:bool", "p:entity,@target?,p:blendSeconds", "success"),
Reg("getmorphweight",   GenericKind.Pure, typeof(MorphGraphInterop), "GetMorphWeightForGraph",   "entity:int", "found:bool,weight:float", "p:entity,@target,>weight", "found"),
Reg("setfacerig",       GenericKind.Exec, typeof(MorphGraphInterop), "SetFaceRigForGraph",       "entity:int", "success:bool", "p:entity,@rig?", "success"),
Reg("playlipsync",      GenericKind.Exec, typeof(MorphGraphInterop), "PlayLipSyncForGraph",      "entity:int,rate:float=1,startSeconds:float=0,strength:float=1", "success:bool", "p:entity,@track,p:rate,p:startSeconds,p:strength", "success"),
Reg("stoplipsync",      GenericKind.Exec, typeof(MorphGraphInterop), "StopLipSyncForGraph",      "entity:int,blendSeconds:float=0.1", "success:bool", "p:entity,p:blendSeconds", "success"),
Reg("queueviseme",      GenericKind.Exec, typeof(MorphGraphInterop), "QueueVisemeForGraph",      "entity:int,startSeconds:float=0,durationSeconds:float=0.1,intensity:float=1", "success:bool", "p:entity,@viseme,p:startSeconds,p:durationSeconds,p:intensity", "success"),
Reg("getlipsyncstate",  GenericKind.Pure, typeof(MorphGraphInterop), "GetLipSyncStateForGraph",  "entity:int", "playing:bool,time:float,length:float,strength:float", "p:entity,>time,>length,>strength", "playing"),
```
Node titles in the palette: Set Morph Weight, Set Face Pose, Clear Morph Weights, Get Morph Weight, Set Face Rig, Play Lip Sync, Stop Lip Sync, Queue Viseme, Get Lip Sync State; target, pose, rig, track and viseme names are attributes (`attr("target","Morph target")` ...), never pins. Methods take primitives only (`int entity, string name, float ...`), `out` parameters for the `>` tokens, `GraphCompiler`/IL needs nothing new. `docs/AVER_NODE_NODES.md` gets a "Morph targets and facial animation" subsection after "Sequencer" (same table shape as the IK section), and the guide `docs/VISUAL_SCRIPTING.md` / `docs/SCRIPTING_API.md` get a short section each; there is also an `.ocasm` `curve` node (2.4), authored in the state machine editor rather than as a graph node.

### 2.10 Editor

- **`SkinPreview` extraction** (`sandbox/src/SkinPreview.hpp/.cpp`): `AnimSkinFeature` (`AnimEditor.cpp:185-341`) moves out unchanged apart from (a) accepting a mesh with no skin streams but with morph targets (`createMesh(..., boneCount = 0, morphKey)`), (b) `stageMorph(std::vector<MorphTargetWeight>)`, (c) `targetNames()`, (d) exposing the decoded mesh's target list. `AnimEditor.cpp` includes it; the Face Rig tab uses the same class (one shared feature registered before the shared preview, as today).
- **`MorphPanel`** (`sandbox/src/MorphPanel.hpp/.cpp`): an ImGui widget over a small interface `{targetNames(), weights in/out, live animated values, rig poses}`. Search filter; per target a slider (range -1..2, `Ctrl+Click` types, double click resets) with the live animated value drawn as a marker when a clip drives it (the slider then writes an override that wins until "Release"); `Zero all`, `Reset to defaults`, `Solo`; a poses tab (rig poses and the 52/15 built-ins) with sliders; `Key` button -> adds a key at the playhead to the clip's `morph.<name>` curve (clip tab) through the same edit path `drawCurves` uses (`AnimEditor.cpp:1274`), or, from a level entity, a Property key `CMorphTargets.morph.<name>` through a new thin `SequenceEditor::keyMorph` over the keyer at `SequenceEditor.cpp:802-810`. Stats line: targets, total entries, active count, GPU bytes.
- **Anim editor hosting**: skeleton and clip tabs get a "Morph Targets" section when the bound mesh has targets (the preview mesh is already resolved by `buildPreview`, `:1034`). Curves named `morph.*`/`pose.*` show in `drawCurves` as ordinary curves; a button adds one by picking a target name. The clip tab's scrub drives the panel's live values from `Pose::curves`.
- **Face Rig tab** (`sandbox/src/FaceRigEditor.hpp/.cpp`, factory `makeFaceRigEditor` registered at `SandboxApp.cpp:468`'s neighbour): opens `.ocface`; left: pose list (add, rename, duplicate, delete, "Import ARKit defaults"), a pose's targets table (target combo from the previewed mesh + weight) and joints table (bone combo + delta gizmo fields); centre: the shared preview with the `MorphPanel`; right: viseme grid (15 buttons, hold to preview), a phoneme box ("HH AH L OW", 80 ms each) that plays through `evalVisemeWeights`, "Validate against mesh" (lists names the mesh lacks), "Bake .ocvis to clip..." writing a curve-only `.ocanim`. Edits are undoable with the editor's `SnapshotUndo` (same pattern as `IkRigEditor`). Saves with `writeFaceRig` atomically.
- **Content browser / Details**: icons and "New > Face Rig" for `.ocface` and an icon for `.ocvis` (`SandboxContentBrowser.cpp:2060` table, starter-asset helpers `:758-830`); `CMorphTargets` appended as the LAST placed kind (`PlacedComponentModel.hpp:41-70`, `LevelComponents.hpp:30`, `LevelComponents.cpp:28`: enum before `kTypeCount`, `typeId`, `applyMorphTargets`, capture; record `COMPONENT MorphTargets rig=<path> inherit=1 off=0 weight=1`), Details section with the rig picker, flags and master weight (`SandboxComponentDetails.cpp`). If another wave-9 feature already appended a kind, append after it.
- **Import dialog**: no change; the importer log line reports `N morph target(s)` per mesh and the weight-curve count per clip (`SandboxContentBrowser.cpp:150-191` summary).
- **Tools**: `tools/MakeFace.cpp` (generator like `MakeRig`) writes `content/dev/Face.gltf` (a 24x24 patch "head" with 16 named targets including `jawOpen`, `mouthSmileLeft/Right`, `eyeBlinkLeft/Right`, `viseme_aa`, `viseme_oh`; target names in `mesh.extras.targetNames`; one SPARSE-accessor target; default weights; a looping `weights` animation; a second animation with CUBICSPLINE) and `content/dev/FaceRigged.gltf` (same mesh skinned to a 2-bone jaw rig, so skinned + morph is exercised), then checks the import numerically. `AverAssetC` gains `visbake <track.ocvis> --rig <x.ocface> --out <clip.ocanim> [--fps N]` and prints `morph targets: N` in its import summary (disjoint hunks from the FBX refactor).

### 2.11 Limits and defaults (frozen)

| Constant | Value |
|---|---|
| `kOcMeshMaxMorphTargets` | 512 per mesh |
| `kMorphMaxActive` | 64 per dispatch (top by `|w| * maxLen`) |
| `kMaxPoseCurves` | 256 per `Pose` |
| `kMorphWeightMin/Max` | -4 / 4 |
| `kMorphActiveEps` | 2^-12 |
| `kMorphIdleFrames` | 8 |
| Morph GPU data cap | 128 MiB per mesh (above: logged, mesh draws unmorphed) |
| Face rig | 256 poses, 32 targets and 16 joints per pose |
| `.ocvis` | 65 536 events |
| Lip-sync defaults | lead 0.04 s, attack 0.05 s, release 0.08 s, minHold 0.06 s, maxTotal 1 |
| Script blend default | 0 s for set/clear; stop-lip-sync 0.1 s |

### 2.12 Traps found while reading (so the implementer does not rediscover them)

1. `Pose` blends are four functions plus **seven `.local.swap(` sites**; miss one and weights vanish after a slot or layered node.
2. `restPose` is the seed for every sampler; it must clear `curves`.
3. glTF morph exporters use **sparse accessors**; the importer refuses them today (`:224`).
4. `importAnimations` returns before doing anything when there is no skeleton (`:973`); a face with no rig has weight animation only.
5. `OcAnimation::valid()` and `writeOcAnim` refuse zero tracks; a curve-only clip needs both relaxed.
6. `nrmXf` is not a true inverse-transpose scale; normal DELTAS need the `1/L` correction (2.3).
7. `SkinnedScene` only walks the `CSkeletalMesh` pool and returns nullptr while the rig size is unknown (`:116`); morph-only needs its own pool walk.
8. Tier-1 binding rule: every declared SRV needs a descriptor, so unused morph/bind/bone slots bind dummies.
9. The per-instance ring (bones AND active list) must live on the instance (`SkinningPass.hpp:40-46`).
10. `simplifyMesh` and `mergeAll` renumber vertices; `remapMorphTargets` / `appendMorphTargets` are mandatory call sites.
11. Montages and `driveClip` need a skeleton; unskinned faces animate via `CAnimator` or Property tracks.
12. `CMorphTargets.weight` zero-fill: 0 reads as 1.
13. First-frame ordering: `AnimSystem::tick` runs before `SkinnedScene::update`, so declared targets/defaults arrive one frame late.
14. An `.ocasm` using the `curve` node is unreadable by older builds.

## 3. FILE OWNERSHIP

Paths are relative to the repo root. Shared-file edits are marked with `[wave9/morph begin] ... [wave9/morph end]` in CMake and kept additive.

### 3.1 Files only this feature touches

CREATE:
- `modules/formats/include/aver/formats/MorphTarget.hpp`, `modules/formats/src/MorphTarget.cpp` (builder, remap, append, curve helpers, constants).
- `modules/anim/include/aver/anim/Morph.hpp`, `modules/anim/src/Morph.cpp` (CPU reference `applyMorphTargets`, `morphBoundsPad`, active-list helpers shared with tests).
- `modules/anim/include/aver/anim/FaceRig.hpp`, `modules/anim/src/FaceRig.cpp` (`.ocface` asset, defaults, ARPAbet table, expansion, joint application).
- `modules/anim/include/aver/anim/Viseme.hpp`, `modules/anim/src/Viseme.cpp` (`.ocvis`, `evalVisemeWeights`, `bakeVisemeTrack`).
- `modules/anim.scene/include/aver/anim/MorphSystem.hpp`, `modules/anim.scene/src/MorphSystem.cpp` (component, system, provider).
- `modules/framework/include/aver/framework/framework_morph_abi.h`, `modules/framework/src/MorphAbi.cpp`.
- `modules/render.skin/include/aver/render/MorphLayout.hpp`, `modules/render.skin/src/MorphLayout.cpp`.
- `scripting/csharp/Aver.Framework/Morph.cs`, `MorphNative.cs`, `FaceShapes.cs`.
- `scripting/csharp/Aver.Graph/GraphMorphNodeTable.cs`, `scripting/csharp/Aver.Graph.Tests/MorphNodeTests.cs`.
- `sandbox/src/GraphNodeDefsMorph.hpp`, `sandbox/src/SkinPreview.hpp`, `sandbox/src/SkinPreview.cpp`, `sandbox/src/MorphPanel.hpp`, `sandbox/src/MorphPanel.cpp`, `sandbox/src/FaceRigEditor.hpp`, `sandbox/src/FaceRigEditor.cpp`.
- `tools/MakeFace.cpp`; generated, committed `content/dev/Face.gltf`, `content/dev/FaceRigged.gltf`, `content/dev/Face.ocface`.
- Tests: `tests/formats/src/MorphFormatTest.cpp`, `tests/formats/src/MorphGltfTest.cpp`, `tests/anim/src/PoseCurveTest.cpp`, `tests/anim/src/MorphTest.cpp`, `tests/anim/src/FaceRigTest.cpp`, `tests/anim/src/VisemeTest.cpp`, `tests/anim.scene/src/MorphSystemTest.cpp`, `tests/render.skin/CMakeLists.txt`, `tests/render.skin/src/MorphLayoutTest.cpp`, `tests/framework_morph/CMakeLists.txt`, `tests/framework_morph/src/FrameworkMorphAbiTest.cpp`.
- Docs: `docs/ANIM_MORPH_FACE.md` (user guide written with the implementation).

MODIFY (this feature is the only expected toucher):
- `modules/formats/include/aver/formats/OcMesh.hpp`, `modules/formats/src/OcMesh.cpp` (struct, flag, macro, `MRPH` read/write, `valid()`).
- `modules/formats/src/GltfImport.cpp` (sparse accessors, targets, weights clips, notes).
- `modules/formats/include/aver/formats/OcAnim.hpp`, `modules/formats/src/OcAnim.cpp` (prefix constants, `OcCurve::poseId`, curve-only validity/write).
- `modules/anim/include/aver/anim/Pose.hpp`, `modules/anim/src/Pose.cpp` (curves, swap, blend semantics, `restPose`, `posedBounds` pad).
- `modules/anim/include/aver/anim/AnimSampler.hpp`, `modules/anim/src/AnimSampler.cpp` (`sampleClipCurves`, curve fill).
- `modules/anim/include/aver/anim/AnimPipeline.hpp`, `modules/anim/src/AnimPipeline.cpp`, `modules/anim/src/AnimGraphAsset.cpp`, `modules/anim/src/AnimStateMachine.cpp` (curve node; overlay mode; swap).
- `modules/anim/src/Montage.cpp`, `modules/anim.scene/src/AnimMontages.cpp` (swap only).
- `modules/anim.scene/include/aver/anim/AnimSystem.hpp`, `modules/anim.scene/src/AnimSystem.cpp` (member, hooks, register, clear, tick calls, swap, overlay mode).
- `modules/render.skin/shaders/skin.hlsl`, `modules/render.skin/include/aver/render/SkinningPass.hpp`, `modules/render.skin/src/SkinningPass.cpp`, `modules/render.skin/include/aver/render/SkinnedScene.hpp`, `modules/render.skin/src/SkinnedScene.cpp`, `modules/render.skin/include/aver/render/SkinSelfTest.hpp`, `modules/render.skin/src/SkinSelfTest.cpp`, `modules/render.skin/CMakeLists.txt`.

### 3.2 SHARED files (small, additive edits; other wave-9 features may also touch them)

| File | Edit |
|---|---|
| `modules/formats/CMakeLists.txt` | add `src/MorphTarget.cpp` |
| `modules/anim/CMakeLists.txt` | add `src/Morph.cpp src/FaceRig.cpp src/Viseme.cpp` |
| `modules/anim.scene/CMakeLists.txt` | add `src/MorphSystem.cpp` |
| `modules/framework/CMakeLists.txt` | add `src/MorphAbi.cpp` (beside `IkAbi.cpp`, line 22) |
| `CMakeLists.txt` (root) | `add_subdirectory(tests/render.skin)` (guarded like `:595`), `add_subdirectory(tests/framework_morph)` (beside `:578`) |
| `tests/formats/CMakeLists.txt`, `tests/anim/CMakeLists.txt`, `tests/anim.scene/CMakeLists.txt` | new executables; `MakeFace` registered beside `MakeRig` (`:252-263`) |
| `sandbox/CMakeLists.txt` | add `SkinPreview.cpp MorphPanel.cpp FaceRigEditor.cpp` beside `IkRigEditor.cpp` (`:88-89`) |
| `modules/trifactor/src/ClusterBuilder.cpp` | `simplifyMesh` (`:1628-1648`): remap `morphTargets` with `remapMorphTargets` |
| `tools/AverAssetC.cpp` | `mergeAll` (`:767`) `appendMorphTargets`; `visbake` subcommand; import summary line (disjoint from the FBX refactor hunks) |
| `Runtime/src/SequenceTracks.cpp` | virtual Property fields + release (2.5) |
| `Runtime/src/GameApp.cpp`, `sandbox/src/SandboxApp.cpp` | provider install (1 line each); `SandboxApp.cpp` also `assetEditors_.registerFactory(&editor::makeFaceRigEditor)` |
| `Runtime/include/aver/game/GameSystemsWiring.hpp` | `anim::animSystem().morphs().clearRuntime()` in `resetGameSystems` |
| `Runtime/src/LevelComponents.cpp`, `Runtime/include/aver/game/LevelComponents.hpp`, `sandbox/src/PlacedComponentModel.hpp`, `sandbox/src/SandboxComponentDetails.cpp` | append the `MorphTargets` placed kind |
| `sandbox/src/AnimEditor.cpp` | remove `AnimSkinFeature` (now `SkinPreview`), host `MorphPanel`, stage weights, curve helper button |
| `sandbox/src/SequenceEditor.cpp`, `.hpp` | `keyMorph` wrapper |
| `sandbox/src/SandboxContentBrowser.cpp` | icons, New > Face Rig, import summary line |
| `sandbox/src/GraphNodeDefs.hpp` | one include, one `appendMorphNodeDefs(t)` call |
| `scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs` | one `.Concat(MorphTable())` |
| `scripting/csharp/Aver.Graph.Tests/Program.cs` | `failures += MorphNodeTests.RunAll();` |
| `sandbox/src/AnimStateMachineEditor.cpp` | one row for the `curve` pipeline node kind |
| `docs/AVER_NODE_NODES.md`, `docs/SCRIPTING_API.md`, `docs/VISUAL_SCRIPTING.md`, `docs/formats/FORMAT_SPECS.md` (5.10, 9 curve naming, 10c property virtual fields, 11j/11k), `docs/ANIM_MONTAGES.md` (curve node), `CHANGELOG.md` | additive sections |

No change to `scene_abi.h`, `Components.hpp`, `framework_abi.h`, `framework_anim_abi.h`, or any `module.json` (render.skin already depends optionally on `Aver.Anim.Scene`).

## 4. Phased implementation

Each phase builds, passes its tests and leaves the tree shippable.

**Phase 1 - Formats and import.** `MorphTarget.hpp/.cpp`, `OcMorphTarget`, `MRPH` read/write, `contentVersion`, sparse accessors, glTF targets and weights clips, curve-only clips, `simplifyMesh` remap, `mergeAll` append, `unsupported` text. Test: `MorphFormatTest`, `MorphGltfTest`, existing `MeshTest`/`GltfTest`/`TrifactorTest` unchanged. Result: meshes and clips carry morph data; nothing renders it. FBX can attach through the builder.

**Phase 2 - Pose curves, CPU reference, graph curve node.** `Pose::curves`, swap sites, blend rules, `sampleClipCurves`, `Morph.hpp` (CPU morph, bounds pad, active-list helper), `posedBounds` pad, `.ocasm` `curve` node (+ editor row). Test: `PoseCurveTest`, `MorphTest`, `AnimPipelineTest` additions. Result: weights flow through every blend; CPU parity function exists.

**Phase 3 - GPU.** `skin.hlsl`, `MorphLayout`, `SkinningPass` (pipeline, dummies, shared morph buffers, active ring, morph-only meshes), `SkinnedScene` (declare, stage, morph-only pool walk, idle fallback, bounds pad, `morphTargetNames`), `SkinSelfTest` morph cases. Driven for now by a debug source (`MorphSystem` stub reading `Pose::curves`). Test: `MorphLayoutTest` (CPU twin of the shader), headed `--skin-test` on D3D12 and `--backend vulkan`. Result: a skinned or unskinned mesh visibly morphs from clip curves.

**Phase 4 - Runtime.** `CMorphTargets`, `MorphSystem` with layers, `AnimSystem` hooks (evaluate, loose, inherit, register, clear), unskinned animator path, provider, sequencer virtual fields, placed component. Test: `MorphSystemTest`. Result: script/sequencer/animation weights all work end to end (still no assets for faces).

**Phase 5 - Face rig and lip-sync.** `FaceRig`, `Viseme`, defaults, joint poses, lip-sync player, `bakeVisemeTrack`, `AverAssetC visbake`. Test: `FaceRigTest`, `VisemeTest`, `MorphSystemTest` additions.

**Phase 6 - Script surface.** `framework_morph_abi.h`, `MorphAbi.cpp`, provider install, C# `Morph.cs`/`MorphNative.cs`/`FaceShapes.cs`, nine nodes, palette mirror, node tests, docs (`AVER_NODE_NODES.md`, `SCRIPTING_API.md`, `VISUAL_SCRIPTING.md`). Test: `FrameworkMorphAbiTest`, `MorphNodeTests`.

**Phase 7 - Editor and content.** `SkinPreview` extraction, `MorphPanel`, Anim editor hosting, `FaceRigEditor`, Details/Add Component, content browser, `SequenceEditor::keyMorph`, `MakeFace` and the committed dev assets, `docs/ANIM_MORPH_FACE.md`, `FORMAT_SPECS.md`. Headed verification (section 6).

## 5. Tests

All no-GPU `*Test` executables, auto-registered by `aver_register_ctest_in` (add `add_executable` only; do not add `add_test`).

- **`MorphFormatTest`** (tests/formats): write/read round trip with 3 targets (one without normals, one with a name needing the string table, one at the vertex-0 and last-vertex edges); mesh without targets is byte-identical to the pre-feature writer output (compare against a vector produced with the feature's flag clear and against a stored hash of a fixture); `contentVersion` 2 only with targets; an old-style reader simulation (parse with the chunk removed) returns the base mesh; truncated chunk, offset past the end, non-ascending vertices, vertex >= count, NaN, duplicate names, 513 targets, `VertexCount` mismatch, `version = 2` chunk (skipped) all handled as specified; `finalize` extents; `MorphTargetBuilder` (sparsify epsilon, duplicate adds accumulate, `setSourceMap` expansion, empty targets dropped and counted); `remapMorphTargets` with dropped vertices and reordering; `appendMorphTargets` union/pad; `simplifyMesh` on a skinned+morph mesh keeps `valid()` and per-vertex deltas (compare by position).
- **`MorphGltfTest`**: hand-built glTF documents (the `makeGlb`/`b64` helpers in `GltfTest.cpp:29-148` are the pattern): 2 targets with POSITION+NORMAL deltas, `extras.targetNames`, default `weights`; deltas land axis-converted and x100 (+X in glTF becomes engine +Y, scaled), normal delta equals the analytic `normalize(n + dn) - n` through a non-uniform-scale node (the `1/L` rule), a mirrored node reverses nothing wrong; a SPARSE accessor target (base zeros + 2 indices, u16 and u32 index types, normalized values); sparse index out of order/out of range fails with a `why`; two primitives with different target counts; mesh copy for a second basis rotates its deltas; weights animation LINEAR/STEP/CUBICSPLINE -> `morph.<name>` curves with the expected values and tangents; weights-only animation with no skeleton -> curve-only clip that passes `valid()` and round-trips `writeOcAnim/parseOcAnim`; same target name on two nodes keeps one curve; a skinned+morph mesh keeps `hasSkin()`; `unsupported` lists tangent deltas and no longer lists "morph targets".
- **`PoseCurveTest`** (tests/anim): `restPose` clears; `sampleAnimation` fills only prefixed curves (checks `poseId` caching); crossfade vs overlay for `blendPose` (missing on either side, both present, `t` 0/1, size mismatch); `blendPoseMasked` curve weight is the max bone weight; `addPose` rule; cap at 256; **the seven swap sites**: run a pipeline with a slot node, a layered node and an additive node over a pose carrying curves and assert they survive (the regression test for trap 1); a state machine crossfade between a state with `morph.jawOpen` and one without fades it (Crossfade); a montage slot over a base with face curves keeps them (Overlay); an `AnimPlayer` crossfade; `.ocasm` `curve` node parse/write/validate/evaluate (set and add, param-driven), and an unknown node type still fails.
- **`MorphTest`**: `applyMorphTargets` against a brute-force dense reference; weights 0 and negative and >1; empty active list is identity; normal renormalisation; `morphBoundsPad` plus `posedBounds` contains every posed+morphed vertex of a randomised skinned mesh (the argument in `Pose.hpp:119-128` extended); active-list cap keeps the largest `|w| * maxLen`.
- **`MorphLayoutTest`** (tests/render.skin, links `Aver.Render.Skin` + Core only, the `SoftBodySceneTest` shape): rank words and prefix counts for targets covering edge vertices, a full word, an empty target, V not a multiple of 32; `evalMorphLayout` (the C++ twin of the shader loop, same formula) equals `applyMorphTargets` for randomised meshes/active lists within 1e-5; `buildActiveList` order/cap/threshold; 128 MiB refusal; deltaBase prefix sums.
- **`FaceRigTest`**: asset round trip (nine-digit floats, unknown keys ignored), every invalid-file case fails with a `why`; `defaultFaceRig()` has all 52 names and 15 visemes, every viseme target is an ARKit name, the ARPAbet table has 39 entries all resolving; pose resolution order (explicit > mesh target > default) with declared target sets; joint pose application on a real parented rig with non-identity rest rotations (position, rotation order, scale; weight 0.5 = halfway; skipped bone warned once).
- **`VisemeTest`**: `.ocvis` parse/write, unsorted events, bad records ignored, symbol set switch; `evalVisemeWeights` envelope numbers at hand-computed times (before lead, mid attack, hold, release end), overlap max, `maxTotal` normalisation; `bakeVisemeTrack` samples equal direct evaluation within 1e-3 and the clip passes `valid()` (curve-only); queued events extend a track; unknown symbol ignored.
- **`MorphSystemTest`** (tests/anim.scene, no scripting host, no GPU; the `IkRigSystemTest.cpp:163` fixture shape): component registration/fields/`sizeof`; layer order (animation + script override + sequencer override + lip-sync) numbers; blend-in/out ramps; master weight 0 reads as 1, `kMorphOff`; unskinned entity with a `CAnimator` clip plays `morph.` curves; no-animator entity driven by script only (`tickLoose`); `kMorphInheritParent` with a child evaluated before its parent; declared targets filter the frame, defaults fill undriven targets; `frame()` sorted/sparse/revision/moving; lip-sync play/stop/queue/time/state; entity destruction and recycled handle cleanup; `clear()`/`clearRuntime()`; `releaseOwner`; provider ops through `morphProvider` (each op, auto-attach, refusals); a clip with `morph.` curves through a state machine pose source ends in the entity's frame.
- **`FrameworkMorphAbiTest`** (tests/framework_morph, the `FrameworkIkAbiTest` shape): version, zeros without a provider, each function forwards the right op/args to a fake provider, hashes pass through, out-arrays filled.
- **`MorphNodeTests.cs`** (Aver.Graph.Tests, `NodeRowKit` as in `IkNodeTests.cs`): the nine rows exactly (names, pin lists, defaults: `blendSeconds` 0, `rate` 1, `strength` 1, `durationSeconds` 0.1, stop 0.1), only the two state reads pure, missing required attribute fails by name, IL valid under both compilers, a chain `OnTick -> SetMorphWeight -> PlayLipSync` compiles.
- **GPU parity (on device, `--skin-test`)**: `SkinSelfTest` grows from 3 to 5 cases: case 3 morph-only (`noSkin`, 8 active targets, V = 501 not a multiple of 32 or 64, random sparse targets, one target with no normals), case 4 skinned + morph with the existing out-of-range/zero-weight vertices; compared with `skinVertices` after `applyMorphTargets` (positions 1e-3 cm, normals 1e-4, uv exactly) on D3D12 and `--backend vulkan`. Distinct seeds per case so a crossed ring/active buffer shows as a wrong answer.

## 6. Headed manual verification

Build with the normal editor target; the dev assets come from `MakeFace` (no external download).

1. Run `MakeFace` (writes `content/dev/Face.gltf`, `FaceRigged.gltf`, `Face.ocface`). Expect its own numeric self-checks to print `ok`.
2. Editor: Content Browser -> Import `Face.gltf` into a project. Log shows `16 morph target(s)` on the mesh and a curve-only clip with the weight curves; the importer does not report "morph targets" under unsupported. Import `FaceRigged.gltf` the same way: mesh + `.ocskel` + a clip with bone tracks AND `morph.*` curves.
3. Place the unskinned face, Details -> Add Component -> Morph Targets. Open the mesh's clip via a `CAnimator` (Animator component with the weights clip): the face animates (jaw, smile, blink) in the viewport and ray-traced reflections/shadows follow the morph (look for the `[Voxi] mesh N has compute-written vertices` line the first time it goes non-idle). Stop the clip: after ~8 frames the log/profiler shows the entity back on the static path.
4. Anim editor, skeleton tab of the rigged face: Morph Targets section lists 16 targets; dragging `jawOpen` moves the preview; `Key` on a clip tab adds a `morph.jawOpen` key at the playhead and the curve shows in the Curves tab; scrubbing the imported clip moves the sliders' live markers; skinned deformation and morph combine (bend the jaw bone clip while smiling).
5. Face Rig tab (`Face.ocface`): pick a viseme button, the mouth shape appears; type `HH AH L OW` into the phoneme box and watch the sequence play; "Validate against mesh" lists nothing missing; "Bake .ocvis to clip..." produces a curve-only `.ocanim`; assign that clip to the face's Animator and it speaks.
6. Play: a script (or the Graph editor: OnTick -> Set Morph Weight `jawOpen`) drives the weight; `Entity.PlayLipSync` with the baked `.ocvis` plays and `StopLipSync` fades it; `Get Lip Sync State` reads the time. Stop Play: all script overrides are gone (face returns to its clip).
7. Sequencer: key `CMorphTargets.morph.eyeBlinkLeft` as a Property track with three keys; scrub forward and backward: the eye follows exactly; mute the track: the override is released.
8. Skinned + montage: play a body montage in a slot on the rigged face while `Face` curves play from the base clip: the face keeps animating (overlay rule).
9. Both backends: repeat 3 and 4 with `--backend vulkan`; run `--skin-test` on both: all five cases pass with the printed worst position/normal error under the tolerances.
10. Perf: place 50 rigged faces in a grid with the weights clip running; compare frame time with morph disabled (`kMorphOff` on all) and read the `Aver.Skin` GPU pass; note the numbers in the commit message. With ray tracing on, check the BLAS refit time stays flat when only 5 of the 50 are non-idle.
11. Regressions: a plain skinned character without morph looks and costs the same (srvCount 6 dummies bound; compare the skin pass time before/after); a mesh imported before this change still opens; saving an old `.ocmesh` untouched writes the same bytes.

## 7. Open questions

Defaults are chosen; nothing blocks the work.

1. **Weight range**: clamp to `[-4, 4]`. Default stands unless the owner wants a tighter art range.
2. **Auto-attach**: level placements do not get `CMorphTargets` automatically at import; animation-driven weights on a skinned character work without the component, everything else needs it (Add Component, or the first script call attaches it). Default stands.
3. **FBX in-between shapes**: dropped with a report (matches `FBX_IMPORT.md`); revisit when a real asset needs them.
4. **Tangent deltas and per-vertex tangents**: dropped; the engine vertex carries no tangent.
5. **Morphing meshes draw LOD 0 only** in v1; per-LOD posed index buffers are a follow-up with no format change.
6. **Unskinned face + montage**: not supported (montages need a skeleton); Animator clip and Property tracks cover it.
7. **Viseme and ARKit tables are starting art**: the numbers in section 2.6 will be tuned in the headed pass; the structure is frozen.
8. **Patent check**: no live search was run for this design. The techniques are standard public art; the owner may want the same check the sequencer and many-light plans got before shipping lip-sync coarticulation, which is the only part with a plausible patent history.
