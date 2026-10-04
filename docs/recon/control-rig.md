# Control Rig — scout and build plan

A **control rig** is an authorable graph that runs every frame and *modifies a pose* — foot placement
on uneven ground, a head that tracks a target, a hand that stays on a weapon grip while the arm plays
a canned clip. Unreal calls it Control Rig; the shape is the same everywhere: procedural adjustment
layered on top of, or instead of, a sampled clip.

This is a scout. **Nothing here is built.** Every claim below cites a file and was read, not
remembered; where a claim is an inference rather than a reading, it says so.

---

## 1. What already exists, and it is more than expected

**The pose is trivially mutable.** `anim::Pose` is one struct with one field
(`modules/anim/include/aver/anim/Pose.hpp:15-21`):

```cpp
struct Pose { std::vector<Transform> local; };   // one LOCAL transform per bone, bone order
```

Local space, `Transform` = position/rotation/scale, no accessor wall, no dirty flags, no
model-space variant. A rig that wants to move a bone assigns to `local[i]`. There is nothing to
design around here — which is the single most important finding in this document, because it means
a control rig needs no change to the animation data model at all.

**The parent-chain resolve and the skinning multiply are already separate functions.**
`poseToModel` (`Pose.cpp:25-39`) walks bones in stored order relying on the skeleton's
parents-before-children invariant; `poseToSkinning` (`Pose.cpp:51-62`) calls it and multiplies each
result by the bone's `inverseBind`. A rig that needs model space mid-evaluation — every IK solver
does — calls `poseToModel` and does not have to write one.

**Sockets are effector placement, already built.** `socketModelMatrix`
(`Pose.hpp:48`, `Pose.cpp:41-49`) composes a socket's offset against a **posed** model-matrix array,
and `AnimSystem::socketModel` exposes it per entity. An IK goal expressed as "the grip socket on the
weapon" resolves through code that exists.

**Additive layering is implemented, tested — and unreachable.** `addPose`
(`Pose.hpp:58`, `Pose.cpp:75-103`) blends an additive pose relative to a rest pose, and
`tests/anim/src/AnimTest.cpp` exercises it. Verified by grep: **the only callers outside tests are
none.** `AnimSystem::tick` uses `blendPose` and nothing else. This is the repository's recurring
"declared but unread" shape, and it is the exact operation a control-rig layer wants.

## 2. What does not exist

**No IK of any kind.** Verified independently rather than taken from a summary: a case-insensitive
sweep for `fabrik|two.?bone|inverse kinematic|IKChain|IKSolver|IKGoal|lookAt|aimConstraint` over the
whole tree, excluding the vendored Jolt, returns only camera view matrices (`Mat4::lookAtLH`,
`ActorPreview.cpp`'s own `lookAt`) and one behaviour-tree **action name** in `docs/SYNAPSE.md:193`.
No two-bone solver, no FABRIK, no aim or look-at constraint, no foot placement.

**No procedural bone API.** Nothing in `modules/anim` or `modules/anim.scene` sets a bone transform
other than `restPose` and `sampleAnimation`. `Pose::local` is public, so nothing *prevents* it — but
no API offers it, which means no C# and no graph node can reach it either.

**No rig authoring surface.** `sandbox/src/AnimEditor.cpp` authors sockets (`drawSockets`), notifies
(`drawNotifies`) and curves (`drawCurves`). Its bone tree (`drawBones`) is **read-only** — select a
bone, see its model-space position, and that is all. There is nowhere to author a constraint today.

**`modules/anim` has no TODO or FIXME markers at all.** The repository is silent about control rigs
rather than claiming they are impossible — worth stating, because this tree has a documented habit of
"cannot" comments that turned out stale, and there is no such comment to disbelieve here.

## 3. THE SEAM — one place, and it is empty

`AnimSystem::tick`, `modules/anim.scene/src/AnimSystem.cpp:128-148`, per entity:

```cpp
Posed& p = posed_[e];
p.skel = skel;
restPose(*skel, p.pose);                       // seed

if (const fmt::OcAnimation* c = clip(a->clip)) {
    ...
    sampleAnimation(*c, t, p.pose);            // or sample + blendPose at partial weight
}
                                               // <-- NOTHING HERE. This is the whole opportunity.
poseToSkinning(*skel, p.pose, p.skin);         // :147
```

`p.pose` holds the finished local-space pose and goes straight into skinning matrices. **There is no
hook, no interface, no callback between those two statements.** A control rig is a pose modifier
that runs in that gap, and adding one is a change to this function and nothing else.

Two consumers must keep working with a modified pose, and both already run after it:

- `updateAttachments` (`AnimSystem.cpp:154-193`) resolves every `CAttachment` against its parent's
  pose, and is deliberately called after **every** entity's pose exists (`:145-150`). A socket on a
  rig-modified bone therefore follows the rig for free.
- `SkinnedScene::update` copies `AnimSystem::skinning()` (`modules/render.skin/.../SkinnedScene.hpp:49`),
  which is downstream of `poseToSkinning`.

## 4. What an `.ocrig` holds

Proposed, not built. A rig is a **list of operations evaluated in order** against a pose — the same
shape `.ocbt` uses for behaviour trees (parent-before-child array, `valid()` enforcing it), which is
worth copying rather than reinventing:

| Op | Reads | Writes |
|---|---|---|
| `CopyBone` | a bone, a space | a bone |
| `AimAt` | a bone, a target (world / socket / bone), an aim axis, an up axis | that bone's rotation |
| `TwoBoneIK` | root/mid/tip bones, a goal, a pole vector, stretch limits | three bone rotations |
| `SetBone` | a value from a curve or a parameter | one channel of one bone |
| `Blend` | a weight, two sub-results | the blended pose |

Every op names bones **by index resolved from name at load**, the way `OcSocket::bone` already does
— a name in the file, an index at runtime, so a rig survives a skeleton whose bone order changes.

Parameters come in from outside (a float per frame: "how planted is the left foot"). The obvious
source is the curve machinery that already exists — `sampleCurve` (`AnimSampler.cpp:131-154`) reads
named float curves off a clip — plus a small setter so gameplay can drive one directly.

## 5. Build plan

**Slice 1 — the hook, and one solver, with no file format.**
`anim::PoseModifier` as an abstract call in the gap at `AnimSystem.cpp:146`, plus `twoBoneIk(Pose&,
const OcSkeleton&, root, mid, tip, goal, pole)` in `modules/anim`. Pure, no scene, no asset. This is
the smallest thing that is *provably working*: a test poses a three-bone chain, asks for a goal it
can reach, and asserts the tip lands on it within tolerance; asks for one it cannot, and asserts the
chain straightens toward it rather than tearing. **Land this alone.** If the hook is wrong, it is
wrong here, where one function and one test are involved.

**Slice 2 — `.ocrig`, the asset.** Format, loader, `valid()`, and the op list above minus IK-driving
parameters. `modules/formats` gains `OcRig.hpp/.cpp` beside `OcBt`; a golden round-trip test in
`tests/formats` the way every other `.oc*` has one.

**Slice 3 — `CControlRig`, the component.** A rig asset plus per-instance parameter values, applied
through the Slice 1 hook. Registered dynamically at the composition root exactly as
`CSynapseAgent` and friends are (`docs/SYNAPSE.md` §6), so `Components.hpp`, `scene_abi.h` and
`kComponentBuiltinMax` do not change and no ABI version moves.
**Attach through a helper that assigns a fresh value, never `world.addComponent` raw** —
`ComponentPool::add` zero-fills and runs no constructor, so in-class defaults are dead code and a
rig attached the raw way would have a zero weight and do nothing.

**Slice 4 — authoring.** A Rig tab in `AnimEditor`, reusing its existing skinned preview and bone
tree — the bone tree is already there and already read-only, so this is where it earns an editing
mode.

**Slice 5 — reach.** Aver Node nodes (`SetRigParam`, `GetSocketTransform` — note the palette has
neither today, only `AttachToSocket`, `GetAnimCurve`, `SetSkeleton`, `PlayAnimation`) and the
matching C# surface. Adding a node needs **five** edits, not four: the `...ForGraph` bridge in
`GraphInterop.cs` comes first, because `GraphCompiler` resolves every native call through an eager
`?? throw` field and a missing method breaks every graph in the process at static init.

## 6. Decisions this scout does not make

- **Whether a rig replaces or layers on the clip.** `addPose` exists and is unused, which argues for
  layering; but a foot-planting rig wants to *overwrite* an ankle, not add to it. Probably per-op,
  which is a format decision for Slice 2 and should be made with a real rig in hand.
- **Where a rig runs relative to physics.** A ragdoll and a control rig both want the last word.
  Nothing forces the answer yet because there is no ragdoll either.
- **Whether `modules/anim` grows the solver or a new `modules/anim.rig` appears.** Slice 1 is small
  enough to live in `anim`; if Slice 2's op set grows, a module split is the usual answer here.

---

*Ground truth for this document was read on 2026-09-02 against the tree at that date. The two claims
most worth re-checking before building are the seam at `AnimSystem.cpp:146` and that `addPose` still
has no runtime caller — both are one grep each, and both are the sort of thing that quietly stops
being true.*
