# Control Rig — scout, and what has been built

A **control rig** is an authorable graph that runs every frame and *modifies a pose* — foot placement on uneven ground, a head that tracks a target, a hand that stays on a weapon grip while the arm plays a canned clip. Unreal calls it Control Rig; the shape is the same everywhere: procedural adjustment layered on top of, or instead of, a sampled clip.

The scout half of this document (§1-§4) was read against the tree on 2026-09-02; claims cite a file and were read, not remembered. **Slices 1-3 of the build plan (§5) have since been built**: the pose-modifier hook, `twoBoneIk`/`aimAt` (`modules/anim/include/aver/anim/Ik.hpp`, `tests/anim/src/IkTest.cpp`), the text `.ocrig` format (`modules/formats/include/aver/formats/OcRig.hpp`) and `CControlRig`/`ControlRigSystem` (`modules/anim.scene/include/aver/anim/ControlRig.hpp`, `tests/anim.scene/src/ControlRigTest.cpp`; registered and installed in `SandboxApp.cpp:496-497` and `Runtime/src/GameApp.cpp:1672`). Slices 4 and 5 (authoring, reach) are NOT built.

---

## 1. What already existed, and it was more than expected

**The pose is trivially mutable.** `anim::Pose` is one struct with one field (`modules/anim/include/aver/anim/Pose.hpp:15-21`):

```cpp
struct Pose { std::vector<Transform> local; };   // one LOCAL transform per bone, bone order
```

Local space, `Transform` = position/rotation/scale, no accessor wall, no dirty flags, no model-space variant. A rig that wants to move a bone assigns to `local[i]`. There is nothing to design around here — which is the single most important finding in this document, because it means a control rig needs no change to the animation data model at all.

**The parent-chain resolve and the skinning multiply are separate functions.** `poseToModel` (`Pose.cpp:25-39`) walks bones in stored order relying on the skeleton's parents-before-children invariant; `poseToSkinning` (`Pose.cpp:51-62`) calls it and multiplies each result by the bone's `inverseBind`. A rig that needs model space mid-evaluation — every IK solver does — calls `poseToModel` and does not have to write one.

**Sockets are effector placement, already built.** `socketModelMatrix` (`Pose.hpp:48`, `Pose.cpp:41-49`) composes a socket's offset against a **posed** model-matrix array, and `AnimSystem::socketModel` exposes it per entity. An IK goal expressed as "the grip socket on the weapon" resolves through code that exists.

**Additive layering is implemented, tested — and unreachable.** `addPose` (`Pose.hpp:58`, `Pose.cpp:75-103`) blends an additive pose relative to a rest pose, and `tests/anim/src/AnimTest.cpp` exercises it. Verified by grep (re-checked 2026-10: still true): the only callers outside tests are none. `AnimSystem::tick` uses `blendPose` and nothing else. This is the repository's recurring "declared but unread" shape, and it is the exact operation a control-rig layer wants; `OcRig.hpp` records that `addPose` argues for layering while a planted foot argues for overwriting.

## 2. What did not exist (as scouted 2026-09-02)

**No IK of any kind.** Verified by a case-insensitive sweep for `fabrik|two.?bone|inverse kinematic|IKChain|IKSolver|IKGoal|lookAt|aimConstraint` over the whole tree, excluding the vendored Jolt, which returned only camera view matrices (`Mat4::lookAtLH`, `ActorPreview.cpp`'s own `lookAt`) and one behaviour-tree **action name** in `docs/SYNAPSE.md:193`. Since then `twoBoneIk` and `aimAt` exist; there is still no FABRIK, no foot placement and no ground probing.

**No procedural bone API.** Nothing in `modules/anim` or `modules/anim.scene` set a bone transform other than `restPose` and `sampleAnimation`. `Pose::local` is public, so nothing *prevented* it, but no API offered it, so no C# and no graph node could reach it either. The rig modifier is now the one such path; C# and graph nodes still cannot reach it (§5, slice 5).

**No rig authoring surface.** `sandbox/src/AnimEditor.cpp` authors sockets (`drawSockets`), notifies (`drawNotifies`) and curves (`drawCurves`); its bone tree (`drawBones`) is read-only. There is still nowhere to author a constraint (no `.ocrig` code in the editor).

**`modules/anim` has no TODO or FIXME markers at all** (re-checked 2026-10). The repository is silent about control rigs rather than claiming they are impossible, which matters because this tree has a documented habit of "cannot" comments that turned out stale, and there is no such comment to disbelieve.

## 3. THE SEAM — was empty, now hooked

`AnimSystem::tick`, `modules/anim.scene/src/AnimSystem.cpp`, per entity: seed with `restPose`, `sampleAnimation(*c, t, p.pose)` (`:323`; or sample + `blendPose` at partial weight), then

```cpp
if (poseMod_) poseMod_(e, *skel, p.pose, poseModUser_);   // :340
poseToSkinning(*skel, p.pose, p.skin);                    // :342
```

`PoseModifierFn` and `setPoseModifier` are `AnimSystem.hpp:50,78`. Before the hook, `p.pose` held the finished local-space pose and went straight into skinning matrices with no hook, interface or callback between those two statements; a control rig is a pose modifier that runs in that gap, which is why adding one was a change to this function and nothing else. The modifier runs BEFORE `poseToSkinning` so what the rig did is what gets skinned.

Two consumers must keep working with a modified pose, and both already run after it:

- `updateAttachments` (called at `AnimSystem.cpp:353`, defined `:407`) resolves every `CAttachment` against its parent's pose, and is deliberately called after **every** entity's pose exists. A socket on a rig-modified bone therefore follows the rig for free.
- `SkinnedScene::update` copies `AnimSystem::skinning()` (`modules/render.skin/.../SkinnedScene.hpp:49`), which is downstream of `poseToSkinning`.

## 4. The `.ocrig` as built

**Text, not an AVR1 container**, a deliberate break from `.ocbt` next door: a behaviour tree is baked by a tool, a rig is short, hand-authorable and read far more often than written (four ops is a whole rig), and `.ocgraph` made the same call. A rig is a **list of operations evaluated in order** against a pose; `valid()` checks the file (every op names the bones its kind needs, no empty name, every weight in [0,1]) but NOT that the bones exist, since a rig is validated against a file here and against a skeleton at load.

```
OCRIG 1
NAME ArmReach
OP twobone root=shoulder mid=elbow tip=wrist goal=80,0,0 pole=0,-100,20 weight=1
OP aim bone=head at=0,200,150 axis=0,0,1 weight=0.5
```

Ops built: `TwoBoneIk` (root/mid/tip, goal in the skeleton's model space in cm, pole picking the bend) and `AimAt` (one bone, a model-space target, a bone-local axis). The scout's proposed `CopyBone`, `SetBone` and `Blend` ops and parameter-driven values ("how planted is the left foot" fed from `sampleCurve`, `AnimSampler.cpp:131-154`, or a setter) are **not built**; the only runtime inputs are each op's own `weight` and `CControlRig::weight`. Bones are **named in the file and resolved to indices at load**, as `OcSocket::bone` is, so a rig survives a skeleton whose bone order changes; a rig authored against a different skeleton simply does not find its bones (`findBone` returns false, not an error).

**Solver behaviour (`Ik.hpp`):** `twoBoneIk` places the elbow on the pole's side of the root-to-goal line (a triangle with a given base and two sides has two mirrored solutions; point the pole behind an arm and the elbow goes backwards). **An unreachable goal straightens the chain at it rather than failing**, never tearing the limb. It returns false, leaving the pose untouched, on an out-of-range index, a chain that is not parent-linked (`mid`'s parent must be `root`, `tip`'s `mid`), a pose/skeleton bone-count mismatch, or a zero-length segment.

**`CControlRig`** (`rig` = interned asset path resolved through `AnimSystem`'s resolver, `weight` = master scale on top of each op's weight; 0 turns the rig off without detaching) is registered dynamically at the composition root exactly as `CSynapseAgent` and friends are (`docs/SYNAPSE.md` §6), so `Components.hpp`, `scene_abi.h` and `kComponentBuiltinMax` do not change and no ABI version moves. **Attach through `ControlRigSystem::attach`, never `world.addComponent` raw**: `ComponentPool::add` zero-fills and runs no constructor, so in-class defaults are dead code, `weight` would read 0 and the rig would silently do nothing (the failure SYNAPSE.md records for an agent whose `moveSpeedCm` came back zero). One `ControlRigSystem` per process (`controlRigSystem()`), because the `World` is a singleton and two would race to be `AnimSystem`'s single pose modifier, the loser silently doing nothing. A failed rig load is cached so a missing file is not reopened every frame; `appliedLastTick()` lets a test tell "no rig ran" from "a rig ran and did nothing".

## 5. Build plan: status

**Slice 1 — the hook, and one solver, with no file format. BUILT.** `anim::PoseModifier` in the gap, plus `twoBoneIk` (and `aimAt`) in `modules/anim`. Pure, no scene, no asset. The provable test: pose a three-bone chain, ask for a reachable goal and assert the tip lands on it within tolerance; ask for an unreachable one and assert the chain straightens toward it rather than tearing.

**Slice 2 — `.ocrig`, the asset. BUILT** (in `modules/formats`, `OcRig.hpp`/`.cpp`, beside `OcBt`; with `valid()`), minus the IK-driving parameters and the extra ops above.

**Slice 3 — `CControlRig`, the component. BUILT** (§4).

**Slice 4 — authoring. NOT BUILT.** A Rig tab in `AnimEditor`, reusing its existing skinned preview and bone tree; the bone tree is already there and read-only, so this is where it earns an editing mode.

**Slice 5 — reach. NOT BUILT.** Aver Node nodes (`SetRigParam`, `GetSocketTransform` — the palette has neither; no such node exists anywhere in the tree, only `AttachToSocket`, `GetAnimCurve`, `SetSkeleton`, `PlayAnimation`) and the matching C# surface. Adding a node needs **five** edits, not four: the `...ForGraph` bridge in `GraphInterop.cs` comes first, because `GraphCompiler` resolves every native call through an eager `?? throw` field and a missing method breaks every graph in the process at static init. (Rig parameters must exist before `SetRigParam` can.)

## 6. Decisions

- **Whether a rig replaces or layers on the clip.** `addPose` exists and is unused, which argues for layering; a foot-planting rig wants to *overwrite* an ankle. Decided per-op via each op's `weight` (1 overwrites that op's bones, 0 leaves the clip, between blends). `OcRig.hpp` itself says this was **decided without a real rig to test it against** and is cheap to revisit while nothing has authored one.
- **Where a rig runs relative to physics.** A ragdoll and a control rig both want the last word. Nothing forces the answer yet because there is no ragdoll either.
- **Module placement.** The solver lives in `modules/anim`, the component and system in `modules/anim.scene`; no `modules/anim.rig` was needed. If the op set grows, a module split is the usual answer here.

---

*Scout ground truth read 2026-09-02; §2 and §5 status re-checked against the tree 2026-10-04. The claims most worth re-checking before extending are that `addPose` still has no runtime caller and that `AnimSystem.cpp:340` is still the only pose-edit point; each is one grep.*
