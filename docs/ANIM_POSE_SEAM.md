# Script bone poses, pose-to-pose retarget, authored ragdolls and animation sections

Wave 7 of the Godot parity plan ([plans/GODOT_PARITY.md](plans/GODOT_PARITY.md), items P23 to P26). All four are neutral engine features: nothing here is named
for Godot, every default is today's behaviour, and the Godot mapping is the Fabric Connector's.

## 1. The pose seam (P23)

A script can read where every bone of an animated entity ended up, and set bones to a pose.

**Where it sits.** For one entity the pose modifier chain is

    clip / state machine / montage  ->  CControlRig ops  ->  CIkRig  ->  POSE SEAM overrides  ->  ragdoll (chained)

The seam is applied last inside the control rig's modifier (`ControlRigSystem::applyTo`), so a script has the last word over authored motion, and it runs
before the ragdoll, so a simulated pose still wins while one is blended in. Reads see the pose AFTER the whole chain, or, with the "before" selector, the pose
just before the seam's overrides were applied (what a global-pose-override read needs).

**The component.** `CPoseSeam` (dynamic, registered by `ControlRigSystem::registerComponents`, 8 bytes): a master weight (0 reads as 1) and a negative `kPoseSeamOff`
flag. A script never attaches it: the first write does. The overrides themselves are runtime state (not saved), one record per bone: which channels (position,
rotation, scale), the transform, an amount 0..1 and two flags.

| Flag | Meaning |
|---|---|
| `kPoseSeamPosition` / `Rotation` / `Scale` | the channels an override sets; the others keep the pose underneath |
| `kPoseSeamModel` | the values are a model-space transform of the bone; its children keep their local transforms and follow |
| `kPoseSeamOnce` | dropped after the tick that applied it |

Units are the engine's (centimetres, the skeleton's own axes, quaternions xyzw). A write needs the entity's skeleton to resolve; a bone past the end, no channel
or a non-finite value is refused.

**Framework ABI.** `framework_pose_abi.h` (own version, `AVER_FW_POSE_ABI_VERSION 1`; `AVER_FW_ABI_VERSION_MINOR` is untouched): a provider relay in the shape of
`framework_ik_abi.h`. One provider function (`anim::poseProvider`, `user` = the pose seam) takes an op code; the C functions are `aver_fw_pose_bone_count`,
`_find_bone` (by the fnv1a64 of the name), `_bone_info` (name, parent, rest transform), `_read`, `_write`, `_write_range` (consecutive bones in one call),
`_clear`, `_set_weight`, `_set_enabled`, `_override_count`. With no provider every call answers 0 (find: -1). C#: `Entity.PosedBoneCount`, `FindBone`,
`GetBoneInfo`, `GetBonePose`, `GetBoneModelPose`, `SetBonePose`, `SetBoneModelPose`, `SetBonePoses`, `ClearBonePose`, `SetBonePoseWeight`, `SetBonePosesEnabled`
(`scripting/csharp/Aver.Framework/Pose.cs`).

Tests: `PoseSeamTest` (the chain order against the rig and a chained modifier standing for the ragdoll, channels, amount, weight, model space, once, refusals,
the provider, pruning) and `FrameworkPoseAbiTest` (the relay). Headed check for the owner: a script bone pose on a skinned character, with and without a ragdoll.

## 2. Pose-to-pose retarget (P25)

`CPoseFollow` (`PoseFollow.hpp`, dynamic, 24 bytes) makes an entity take another entity's pose across a `.ocretarget` bone map every tick: the same `Retargeter`
the clip bake uses, run on a live pose. It is a pose SOURCE chained in front of the installed ones (state machines, motion matching), so the rig, the IK rig, the
seam and a ragdoll still run on top. `weight` blends over the entity's own pose (a raw zero-filled attach reads 0 = off; `PoseFollowSystem::attach` writes 1).
The source is read as of the last tick when it is posed later in the same tick (one frame behind). Test: `PoseFollowTest`.

## 3. Authored ragdoll data (P24)

The `.ocragdoll` override file beside a skeleton (`docs/physics/RAGDOLL_ANIMATION.md`) gained per-bone settings, all optional:

    bone <name> [shape capsule|sphere] [offset x y z] [mass kg] [friction f] [bounce r] [joint swingtwist|hinge|fixed|ball] ...

`shape sphere` puts a sphere at the middle of the bone's segment; `offset` moves the whole part (bone-local cm); `mass` is that part's own mass and the others share
the rest by volume (applied when the parts first become dynamic, since a kinematic body has no mass to set); `friction` and `bounce` are that body's. A joint kind
is a limit triple of the same swing-twist joint: `hinge` keeps swing1 and locks the rest, `fixed` locks all three, `ball` frees all three to 180 degrees.
No physics ABI changed and a ragdoll without such lines is built exactly as before. Tests: `RagdollBuilderTest`, `RagdollSystemTest`.

Not done: box and convex shapes (the physics ABI builds capsules and spheres) and a part's own collision layer.

## 4. Markers and sections (P26)

`OcAnimation::markers` (a name, a time in seconds and an rgba hint) is an optional `MRKR` chunk of `.ocanim` (a clip without markers writes the same bytes as
before). `PropPlayer` can play a SECTION of the current clip: `playSection(name, start, end, ...)`, `playSectionWithMarkers`, `setSection`,
`setSectionWithMarkers`, `resetSection`, `hasSection`, `sectionStart`, `sectionEnd`. The playhead starts at the section's edge, a looping clip wraps (or bounces)
inside it, a non-looping one ends at its edge. A negative edge is open. `play()` of another animation and `stop()` drop the section; `play()` of the current one keeps
it. The C seam is `propanim_abi.h` version 2 (`aver_panim_marker_*`, `aver_pplayer_*section*`). Test: `PropSectionTest`.
