# Ragdolls on skinned entities

`modules/ragdoll` (target `Aver.Ragdoll`) turns a skinned entity's skeleton into a physics ragdoll and
back. The physics half (bodies, joints, motors) is `aver_phys_ragdoll_*` in
`physics_ragdoll_abi.h`; this module decides what the parts are and how the simulated pose meets the
animated one.

## What it does

- **RagdollBuilder** derives per-bone capsules and joint limits from an `OcSkeleton` bind pose.
- **RagdollSystem** keeps per-entity state (mode, blend weight, blend rate), installs on
  `AnimSystem::setPoseModifier`, and writes the simulated pose over the animated one.
- **C# access** is `Entity.StartRagdoll / StopRagdoll / RagdollState / SetRagdollStrength /
  AddRagdollImpulse` (`scripting/csharp/Aver.Framework/Ragdoll.cs`), relayed through the framework's
  `aver_fw_ragdoll_*` the way animation curves are.

## Modes

| Mode | Parts | Skin shows |
|---|---|---|
| Kinematic (0) | follow the animated pose | the animation |
| Limp (1) | simulated, motors off | the simulation, blended over the animation by the weight |
| Powered (2) | simulated, motors pull toward the animated pose (`SetRagdollStrength`, 0..1, default 0.5) | the simulation |

`StartRagdoll(mode, blendSeconds)` builds the ragdoll from the entity's *current animated pose* (the
physics ragdoll is created in that pose and `set_pose` teleports it there first, so going limp starts
exactly where the character stands), then ramps the write-back weight 0 to 1 over `blendSeconds`.
Calling it again on a live ragdoll changes the mode. `StopRagdoll(blendSeconds)` ramps the weight back
to 0, then destroys the physics ragdoll; `RagdollState` reads `null` from then on. Switching to
Kinematic keeps the simulation running until the weight reaches 0, so the parts do not snap to the
animation while the skin is still showing the simulation.

The ragdoll starts with zero velocity. A character that dies mid-run does not inherit its run; give it
a shove with `AddRagdollImpulse` (kg cm/s, world space, at the part's centre; `AddRagdollImpulse(vec)`
without a bone name pushes the root part).

## Where it sits in the frame

```
physics step
RagdollSystem::tick(dt)      weights, stops, dead entities   (before the animation tick)
AnimSystem::tick
  per entity: sample clip / pose source
              previous pose modifier (the control rig)       <- CHAINED, runs first
              RagdollSystem modifier                          <- target out, simulated pose in
              poseToSkinning
```

`RagdollSystem::install` keeps whatever modifier was already installed and calls it first, so a control
rig still runs and the ragdoll sees the pose the rig produced. Install it **after** the control rig
(`GameApp::onInit` and `SandboxApp::onInit` both do). The chained modifier is read with
`AnimSystem::poseModifier()` / `poseModifierUser()`; if a modifier is installed and that getter is not
there, `install` returns false and installs nothing rather than silently switching the rig off. A caller
can also pass the previous modifier explicitly: `install(anim, world, prevFn, prevUser)`.

In the modifier, for an entity with a ragdoll:

1. The animated pose is taken to world space (`bone model matrix * entity world matrix`) and sent with
   `aver_phys_ragdoll_set_target` (used by kinematic and powered parts).
2. When the weight is above 0 and the parts are simulated, `aver_phys_ragdoll_get_pose` is read, taken
   back to bone-local space (relative to the parent bone, so the entity's scale cancels) and blended
   into the animated pose by the weight: lerp on position, slerp on rotation. Bone scales stay the
   animation's. Bones with no part anywhere above them keep their animated transform.

The entity's own transform is not moved. The skin follows the parts wherever they go; culling bounds
and anything keyed on the entity's origin do not.

## Building the capsules (no bone names)

`buildRagdoll(skeleton, options, overrides)`; every threshold is a fraction of the skeleton's *extent*
(its largest bind-pose bounding-box side), so a 1.7 m biped and a 17 cm toy get the same decisions.

- A bone with children gets a capsule from its origin toward its **longest child**, shortened at each
  end by an inset so neighbouring capsules overlap at the joint by about a radius.
- **No part** (the bone rides its nearest ancestor that has one) when: the segment is shorter than
  `tinyFraction` (3.5%) of the extent (fingers, twist and helper bones); longer than
  `maxLengthFraction` (40%) (a transform node standing on the floor over the hips); or the bone has no
  child (a leaf has no direction of its own).
- **Radius** = `max(0.18 * length, 0.5 * widest sideways spread of the other children)`, clamped to
  1.2%..7% of the extent. The spread term is what gives a pelvis a radius that covers both hip joints.
- **Limits** default to swing 45/45 and twist 30 degrees: twist is tighter than swing.
- Mass: `BuildOptions::totalMassKg` (70) shared between parts by capsule volume by the physics side.

The entity's uniform world scale at build time multiplies the capsules and radii. Non-uniform scale is
averaged; bone scales are assumed to be 1.

## The override file

If `<skeleton>.ocragdoll` exists beside the skeleton file (same name, extension replaced; found through
the same asset resolver the skeleton is loaded by), it is read when the skeleton's definition is first
built. A malformed file is logged and ignored as a whole. Plain text, one directive per line, `#`
starts a comment, names have no spaces and are the bone names in the `.ocskel`:

```
mass 80                                  # total kg (default 70)
bone Spine1 radius 9                     # cm, replaces the rule
bone Head off                            # no part; it rides its ancestor
bone HeadEnd on length 15 radius 6       # a leaf gets a part: 15 cm along its parent->bone direction
bone LeftLeg limits 20 20 10             # swing1 swing2 twist, degrees
bone LeftHand on radius 5 limits 60 60 30
```

`on` forces a part even for a bone the rule skips; `length` sets the segment length (for a leaf it is
required, because a leaf has no child to point at). A name that matches no bone is ignored, which is
the ordinary case for a file shared between similar rigs. Later lines for the same bone win.
Without a `mass` setting the physics ABI shares `totalMassKg` by capsule volume.

Authored per-part data (wave 7, [ANIM_POSE_SEAM.md](../ANIM_POSE_SEAM.md) section 3), all optional, on the same `bone` line:

```
bone Head shape sphere mass 5.5 friction 0.9 bounce 0.2     # a sphere at the middle of the segment; this part weighs 5.5 kg
bone LeftThigh offset 1 2 3 joint hinge limits 60 0 0       # the part moved (bone-local cm); a hinge keeping swing1
bone LeftKnee joint fixed                                   # all three limits locked
bone UpperArm joint ball                                    # all three limits free (180 degrees)
```

`mass` is that part's own mass (the other parts share the rest of the total by volume) and is applied the first time the parts turn dynamic; `friction` and
`bounce` are that body's. A joint kind sets the limit triple of the same swing-twist joint; `limits` gives the hinge its angle. A box, convex or trimesh
shape is not available: the physics ABI builds capsules and spheres.

## Hosts

Both composition roots (`Runtime/src/GameApp.cpp`, `sandbox/src/SandboxApp.cpp`) install the system
after the control rig, hand the provider functions to `aver_fw_set_ragdoll_provider`, call
`ragdollSystem().tick(dt)` immediately before `animSystem().tick`, and call `ragdollSystem().clear()`
before the physics world is torn down (the sandbox also clears on Stop). Building is gated by
`AVER_MODULE_RAGDOLL` (default ON) and only happens with the scene and physics modules on.

## Limits and how to read a failure

- The entity needs `CSkeletalMesh` and `CAnimator` (the seam only runs for animated, skinned entities).
- `StartRagdoll` returns false for an entity with no skeleton, a skeleton with no bone large enough, or
  a host with no ragdoll support; a physics refusal is logged with the physics error code.
- Definitions are cached by skeleton asset id; `RagdollSystem::clearCache()` forgets them after a
  skeleton or its `.ocragdoll` changes on disk (live ragdolls keep theirs).
- Tests: `tests/ragdoll` (`RagdollBuilderTest` is pure arithmetic; `RagdollSystemTest` runs a real
  physics world with no GPU: seam chaining, the world/local round trip for a moved, turned and scaled
  entity, a limp fall onto a floor, the blend ramp, stop restoring the animation, an impulse).
