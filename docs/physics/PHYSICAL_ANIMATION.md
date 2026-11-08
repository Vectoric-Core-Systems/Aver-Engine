# Physical animation: hit reactions, partial-body simulation, death and getting up

The layer above `RagdollSystem` (`docs/physics/RAGDOLL_ANIMATION.md`) that makes a ragdoll behave like a physical animation component: a hit knocks one part of the body into the physics and the animation takes it back, an arm can be simulated while the rest follows the clip, each bone has its own strength, and a character can die and get up. Module `Aver.Ragdoll`; design in `docs/plans/WAVE2_UE.md` section W6.

Everything is general: nothing is keyed on a bone name or a game. Bone names appear only in a `.ocphysanim` file and in the arguments a script passes.

## The pieces

| Piece | Where | What it is |
|---|---|---|
| `HitEvent` | `modules/core/.../HitEvent.hpp`, C# `Aver.Framework.HitEvent` | One description of a hit: entity, bone hash, impulse (kg*cm/s), world point, damage, radius (> 0 = radial). The weapon, explosion or converted-game code fills it in. Fracture (W7) consumes the same type. |
| `.ocphysanim` | `PhysAnimProfile.hpp` | The profile asset: regions, drive, envelope, death and get-up. Text; round trip and validation against a skeleton. |
| `HitReaction` | `HitReaction.hpp` | Pure envelope math. No world, no physics. |
| `RagdollLife` | `RagdollLife.hpp` | The Alive / Dying / Dead / GettingUp machine and the get-up montage rule. |
| `RagdollSystem` | `RagdollSystem.hpp` | Puts them on a live ragdoll: per-bone blend, per-part drive, pins, `applyHit`, `simulateSubtree`, `blendPhysicalAnimation`, `die`, `getUp`. |
| ABI | `physics_ragdoll_abi.h` (additive) | `aver_phys_ragdoll_set_part_strength / _damping / _torque_scale / _pinned`, `_part_pinned`. |
| Relay | `framework_physanim_abi.h`, C# `PhysicalAnimation.cs`, graph nodes | `Entity.ApplyHit`, `BlendPhysicalAnimation`, `SimulateBoneSubtree`, `SetBoneStrength`, `DieAsRagdoll`, `GetUpFromRagdoll`, ... and the `PhysAnim` node family (`docs/nodes/PhysAnim.md`). |

## The profile

```
physanim brawler
reach falloff 0.55 hops 4 epsilon 0.01 impulse 3000
life death 0.6 getupblend 0.5 supine <montageId> prone <montageId> side <montageId>
default strength 0.5 blend 1
region arm subtree UpperArmL strength 0.2 boost 0.5 attack 0.1 hold 0.1 decay 0.4 impulse 2
region head bone Head bone Neck strength 0.7 decay 0.2
```

A region names bones exactly (`bone`) or a bone and everything below it (`subtree`); a later region wins for a bone both name. Per region: `strength` (powered motor strength), `damping`, `torque` (multiplier on the motor's torque ceiling), `blend` (weight of the simulated pose at a full reaction), `boost` (strength added at a full reaction), `impulse` (multiplier on a hit's impulse), `attack` / `hold` / `decay` (seconds). Bones in no region take `default`. `reach impulse` is the impulse that gives a full reaction. Montage ids are the montages' ObjectIds. `validatePhysAnimProfile` reports unknown bones, empty or duplicate regions and out-of-range values.

## Hit reactions

`applyHit(HitEvent)` on an entity with no ragdoll starts a Powered one for the reaction. The hit bone gets a reaction of `|impulse| / reach impulse` (capped at 1), its neighbours along the skeleton a share that falls by `falloff` per link up to `hops`. Each bone's reaction follows its region's attack, hold and decay; the bones that are reacting are simulated, every other part is pinned to the animation and shows no simulated pose. The hit part receives the impulse (scaled by its region). A second hit on a bone continues from the envelope's current value and the total caps at 1. When every envelope has run out the ragdoll is removed and the pose is the animation again; nothing needs to be cleaned up.

A ragdoll the caller started (`StartRagdoll`) or a dying one is not touched: the hit only pushes its part. A radial event (`radius > 0`) reacts every part inside the reach, weaker with distance, and pushes each away from the point.

The envelopes are deterministic (fixed float order, no random), so the same hits give the same reaction on every machine.

## Partial-body simulation

`aver_phys_ragdoll_set_part_pinned` keeps a part kinematic in Limp and Powered mode: it is carried to its target every step while the unpinned parts simulate and stay joined to it (Jolt constraints between a kinematic and a dynamic body are ordinary). `simulateSubtree(bone)` pins everything outside the bone's subtree and sets those bones' blend to 0. The builder needed no change and the fallback in the plan (whole-body powered ragdoll with high strength outside the region) is not used. The root part is not driven in Powered mode (`physics_ragdoll_abi.h`): a simulated region that includes the pelvis is free to move.

## Per-bone drive and blend

The simulated pose is written over the animation with weight `weight(e) * boneBlend[bone]` (default 1 per bone; the ragdoll-wide weight keeps its ramp). Strength, damping and torque scale are per part, 'inherit' being a negative strength or damping.

## Death and getting up

`die()` makes the whole body Powered at full weight and ramps the motor strength to zero over the profile's `death` seconds, then goes Limp (`Dead`). `getUp()` from `Dead` classifies how the body lies from where the root part's up and forward axes point now, carried from their directions when the ragdoll was built (rig axes do not matter, but the ragdoll must have been built with the character standing): upright (within 45 degrees of vertical), supine (forward points up), prone (forward points down) or side (forward within 20 degrees of horizontal). It plays the profile's montage for that facing (side falls back to prone) through `AnimSystem::montages()` or the handler set with `setGetUpHandler`, and blends the simulation out over `getupblend`; when the blend is done the ragdoll is gone and the life state reads Alive again. A body that is already upright gets no montage.

The entity's transform is not moved to where the body lies (the ragdoll never moves it), so a get-up that should start where the body fell needs the game to move the entity first.

## Limits

- Not run in a window: the feel of a reaction (profile numbers) is subjective and unverified headed. The tests are headless Jolt on a 12 bone skeleton.
- The editor (profile tab, debug overlay, apply-hit button, W6 step 8) is not written.
- A hit before the entity has been posed once is refused (the ragdoll cannot be built yet).
- The Details panel does not show a profile; the entity gets one by `SetPhysicalAnimationProfile`.

## Tests

`PhysAnimProfileTest` (pure: profile, envelope, life), `PhysicalAnimationTest` (headless Jolt), `FrameworkPhysAnimAbiTest` (relay), `PhysAnimNodeTests` (graph rows), and the existing `RagdollSystemTest`, `RagdollTest`.
