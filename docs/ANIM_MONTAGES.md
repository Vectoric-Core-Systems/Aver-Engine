# Animation montages, slots, layered and additive blends, root motion

Parity feature 26 (`docs/plans/UE_FEATURE_PARITY_PLAN.md`). Design contract: `docs/plans/wave2/MONTAGES_ROOT_MOTION.md`.
This page is what ships: formats, runtime behaviour, the script surface and the limits.

Everything here upgrades the existing animation systems (`AnimSystem`, `AnimGraphSystem`, the state machine
and blend space); there is no second animation system. It ends in a `Pose`, so D3D12 and Vulkan are
untouched.

## 1. Montages (`.ocmontage`)

A montage is a timeline of clip segments with named sections, blend in/out, notifies and root motion,
played imperatively on one **slot** of a skinned entity by a script or a graph node. JSON, UTF-8,
`"format": "ocmontage"`, `"version": 1`. Parsing does not require validity (a half-authored asset must
round trip); the runtime calls `valid()` before binding. A newer version is refused.

```json
{
  "format": "ocmontage", "version": 1,
  "name": "SwordCombo", "slot": "DefaultSlot",
  "blendIn": 0.15, "blendOut": 0.2, "blendInCurve": "smooth", "blendOutCurve": "smooth",
  "blendOutTrigger": -1, "rate": 1, "additive": "auto",
  "segments": [
    {"clip": "Anims/slash1.ocanim", "start": 0, "end": -1, "rate": 1},
    {"clip": "Anims/slash2.ocanim", "start": 0.1, "end": 0.9, "rate": 1.2}
  ],
  "sections": [
    {"name": "Windup",  "start": 0.0},
    {"name": "Slash",   "start": 0.4, "next": "Recover"},
    {"name": "Recover", "start": 0.9, "next": "@end"}
  ],
  "notifies": [
    {"time": 0.5, "name": "HitStart", "duration": 0.1},
    {"time": 0.7, "name": "Whoosh"}
  ]
}
```

- **Timeline.** Montage time (seconds, at rate 1) is the segments end to end. A segment plays clip seconds
  `[start, end]` (`end < 0` = the clip's length) at its `rate`, so it occupies `(end - start) / rate`. A
  clip's own loop flag is ignored. A segment plays `[start, end)`: the instant it ends belongs to the next.
- **Sections** partition the timeline: section `i` runs from its `start` to the next one's, the last to the
  end. The first must start at 0 (a montage with none gets `Default`). `next` empty = the following section;
  `"@end"` = the montage ends when this section does; a section's own name = loop.
- **Blend.** `blendIn` / `blendOut` in seconds (0 = a cut), curves `linear` or `smooth` (smoothstep).
  `blendOutTrigger` is how long before the end (of the montage, or of an `@end` section) the blend-out
  starts; -1 = `blendOut`. `rate` is the asset's speed; the instance rate multiplies it. 0 pauses, negative
  clamps to 0 (no reverse playback).
- **Additive.** `"auto"` (default) = additive when the first clip carries the **Additive** flag; `true` /
  `false` override. An additive montage is applied as `addPose(base, sample, firstFrameOfFirstSegment, 1)`.
- **Notifies.** A `duration > 0` makes a notify state (`<Name>_Begin` ... `<Name>_End`). Instant notifies
  embedded in a segment's clip also fire, once, including on a seam between two segments. Notify *states*
  embedded in a clip are ignored inside a montage; montage-level states are always closed on stop, end,
  entity death and `AnimSystem::clear()`.

### Slots

A slot is a name. Per entity per slot the newest montage wins and the previous one blends out
(`Interrupted`), at most four are kept per slot. Where in the pose chain a slot is applied is decided by
the entity's state machine:

- A `slot` or `layered` node in the `.ocasm` **pipeline** (below) applies that slot there.
- `DefaultSlot` montages no node consumed are applied full-body over the finished pose.
- A montage in any other slot with no node for it is **ignored** and warned about once, so an `UpperBody`
  montage never silently plays full-body on a graph without that slot.

Montages sit before the control rig and the ragdoll, so IK and ragdoll blending layer on top of them.

### Events

Through the same notify sink as clip notifies, so `OnEvent` / `CustomEvent` nodes and `Entity` handlers
receive them with no new wire:

| Event | Name |
|---|---|
| notify | `Name` |
| notify state | `Name_Begin`, `Name_End` |
| lifecycle | `<Montage>_Started`, `_Interrupted`, `_BlendOut`, `_Ended` |
| section entry | `<Montage>_Section_<Section>` |

`<Montage>` is the asset's `name`, else the file name without extension. `Ended` always fires exactly once
per play.

## 2. The `.ocasm` pipeline (version 2)

An optional list of nodes applied, in order, to the state machine's pose. Written as version 2 only when a
pipeline exists; without one the file is version 1, byte-identical to before. Older builds refuse a
version 2 file.

```json
"pipeline": [
  {"type": "slot", "slot": "DefaultSlot"},
  {"type": "layered",
   "source": {"kind": "slot", "slot": "UpperBody"},
   "branches": [{"bone": "spine_01", "depth": 3, "weight": 1.0}, {"bone": "hand_l", "weight": 0.0}],
   "weight": 1.0, "weightParam": "upperAlpha"},
  {"type": "layered", "source": {"kind": "clip", "asset": "Anims/aim.ocanim", "loop": true, "speed": 1},
   "branches": [{"bone": "spine_01"}], "weight": 1.0},
  {"type": "additive",
   "source": {"kind": "blendspace", "asset": "Anims/lean.ocblend", "xParam": "lean", "yParam": ""},
   "ref": {"kind": "firstFrame"}, "weight": 1.0, "weightParam": "leanAmount"}
]
```

- **`slot`**: applies the slot's montage pose full-body, weighted by the slot's coverage.
- **`layered`**: a per-bone blend of `source` over the running pose. `source.kind` is `slot`, `clip` (its own
  clock, `loop`, `speed`) or `blendspace` (`asset`, `xParam`, `yParam` read from this machine).
- **`additive`**: adds `source`'s difference from a reference frame. `ref.kind`: `firstFrame` (default),
  `rest`, or `{"kind": "clip", "asset": ..., "time": ...}`.
- **Branch filter.** A bone's mask is the `weight` of its **nearest** ancestor-or-self entry bone, times
  `ramp = depth > 0 ? min(1, (k + 1) / depth) : 1` where `k` is the bone distance from that entry. A
  weight-0 entry carves a hole out of a wider one; a bone under no entry is 0. A name the skeleton lacks is
  skipped (warned once), not an error. The per-bone weight is `mask * weight * (slot coverage)`, so bones the
  source does not animate keep what was under them.
- **`weightParam`** names a float parameter of the machine that multiplies `weight`; an unknown name reads 1.
  Drive it with `SetAnimParamFloat`.

- **`curve`** (wave 9): sets or adds to ONE pose curve, with no source clip, so a machine parameter can drive a
  facial expression or a blend-shape weight without a script. See `docs/ANIM_MORPH_FACE.md`.

  ```json
  {"type": "curve", "curve": "pose.smile", "valueParam": "joy", "value": 1.0,
   "mode": "set", "weight": 1.0, "weightParam": "faceAlpha"}
  ```

  `curve` is `morph.<target>` or `pose.<name>`. The value is `value`, or the named float parameter when
  `valueParam` is set (an unknown name reads `value`). `mode` `set` (default) moves the running curve toward
  the value by `weight`; `add` adds `value * weight` to it. `weight` and `weightParam` behave as on the other
  nodes. A curve node lives in the same list as the others, so it runs in order: put it before a `slot` node
  and a montage's face curves overlay it; after, it overwrites them. Version 2 files with a curve node are
  refused by builds without this feature, like any other pipeline file.

Not in v1: a second state machine as a layer source, mesh-space rotation blend.

## 3. Clip flags (`.ocanim`, no byte change)

- **Additive** (`kOcAnimAdditiveBase`, bit 1): "this clip is authored as an additive delta". Read by a
  montage's `"additive": "auto"`. The constant keeps its old name.
- **Root motion** (`kOcAnimRootMotion`, bit 2): "extract this clip's root bone motion when the entity's
  `CRootMotion` asks". A clip without it is never extracted, so existing content is unchanged.

The Anim tab labels them **Additive** and **Root motion**; with Root motion set it shows what the clip would
move an entity by (`root moves X cm, Y cm, yaw D deg over T s`) and offers a session-only **Lock root in
preview** checkbox.

## 4. Root motion

A dynamically registered component, `CRootMotion` (no change to `Components.hpp` or the scene ABI):

| Field | Meaning |
|---|---|
| `mode` | 0 None, 1 Ignore, 2 Apply, 3 Accumulate. Zero-filled storage is None, so a raw attach changes nothing. |
| `sources` | bit 0 state machine / animator clip, bit 1 montages; 0 reads as both |
| `axes` | bit 0 X, 1 Y, 2 Z, 3 yaw; 0 reads as X, Y and yaw |
| `flags` | bit 0 External: a physics character consumes the accumulator |
| `scale` | multiplier on translation; 0 reads as 1 |
| `accumX/Y/Z`, `accumYaw`, `accumSeconds` | the accumulator: cm in the entity's own frame at the start of the period, radians, seconds. Writable: write zero to clear. |

Extraction is relative planar motion (X, Y, optionally Z, and yaw) of the skeleton's root bone from clips
flagged Root motion. The extracted components are **locked to the skeleton's rest value** in the pose, so the
mesh stays on the entity while the entity moves by the deltas. Pitch and roll of the root are never touched.
It works for `CAnimator` clips, state machines (layer-weighted), blend spaces and montages.

- **Ignore**: the root is locked and the motion thrown away.
- **Accumulate**: the root is locked and the motion collects in the accumulator for the script; the entity
  does not move.
- **Apply**: the root is locked and the entity follows. A plain entity is moved by the animation system
  (`E o delta`: position by the rotated move, rotation by the yaw). A physics character sets External and
  `AverCharacter.Drive` turns the accumulator into capsule velocity (and turns the character), overriding
  input only while the root motion moves it. With External set and nobody consuming for 0.5 s, the
  accumulation is dropped.
- **Mixing.** A flagged montage pulls the base delta toward its own by coverage times the pipeline's keep
  weight (1 for a slot node; the root bone's branch weight for a layered node). A montage with no
  root-motion clip does not override the base.
- **Scrub guard.** An external write to `CAnimator.time`, or a clip change, produces no delta that tick.

Limits: the lock assumes a Z-up engine-space rig whose heading is a rotation about +Z (a root bone authored
with another up axis extracts no yaw: fix it at import); the lock reference is the skeleton rest, so a clip
whose root starts away from rest pops by that offset when extraction turns on; a physics character follows
the animation by one frame (the animation tick runs after gameplay); a child of a scaled parent moves by
the unscaled delta; entities moved by a dynamic rigid body or Synapse agent steering are not handled (those
systems overwrite `CLocal`).

## 5. Script surface (`Aver.Framework`)

```csharp
entity.PlayMontage("Montages/SwordCombo.ocmontage", rate: 1f, section: "Slash", slot: null, blendIn: -1f);
entity.StopMontage();                                  // every montage; or a path
entity.StopMontageSlot("UpperBody", blendOut: 0.1f);
entity.JumpToMontageSection(montage, "Recover");
entity.SetMontageNextSection(montage, "Windup", "Recover");   // null restores the authored link
entity.SetMontageRate(montage, 0f);                    // pause
entity.IsMontagePlaying(montage);                      // null = any
entity.TryGetMontageState(null, out pos, out len, out weight, out rate);
entity.IsInMontageSection(montage, "Slash");

entity.SetRootMotion(RootMotionMode.Apply, RootMotionAxes.Planar, scale: 1f);
entity.ConsumeRootMotion(out Vec3 move, out float yawRadians, out float seconds);   // reads and zeroes
entity.PeekRootMotion(...);
```

Montages are relayed through the framework (`framework_anim_abi.h`, its own version, 1) to
`AnimSystem`'s montage players: the composition root installs `anim::montageProvider` with
`aver_fw_set_anim_provider`. With no provider every call answers false. Root motion needs no ABI: it is
`CRootMotion` over the generic scene field ABI.

Montage players are **transient**: they are not saved, and a load stops them. `CRootMotion` is saved like
any component.

## 6. Graph nodes

Category Animation: `PlayMontage`, `StopMontage`, `StopMontageSlot`, `JumpToMontageSection`,
`SetMontageNextSection`, `SetMontageRate`, `IsMontagePlaying`, `GetMontageState`, `IsInMontageSection`,
`SetRootMotion`, `ConsumeRootMotion`. String inputs (montage, section, slot) are node attributes. Events
and notifies need no node: they arrive by name. Layered and additive weights are machine parameters.

## 7. Editors

- **Montage tab** (`.ocmontage`, create from the Content Browser's *New Animation Montage*): header fields
  (slot, blend in/out with curves, trigger, rate, additive), a timeline with segments, draggable section and
  notify markers (snapped to 1/30 s) and a playhead; a segment list (drop `.ocanim` files from the Content
  Browser to append); a details panel (section name, start, `next`, notify name, time, duration); and a
  simulator that runs a `MontagePlayer` against the real clips with scrub, play/pause, stop, jump-to-section
  and an event log. There is no skinned preview in the tab; the Anim tab previews a clip and Play-in-editor
  shows the montage on a character.
- **State machine tab**: a *Pipeline* section in the details panel (add, reorder and remove nodes; source,
  branch filter with a bone picker taken from the first `.ocskel` under the project's content, reference
  and weight parameter), with the problems it can detect listed (a slot consumed twice, an empty branch
  list, an unknown parameter, a node with nothing to play).

## 8. Tests

`MontageTest`, `RootMotionTest`, `AnimPipelineTest` (pure, `tests/anim`); `MontageSystemTest`,
`RootMotionSystemTest` (`tests/anim.scene`); `FrameworkAnimAbiTest` (`tests/framework_anim`);
`MontageEditorTest` and the pipeline cases of `AnimGraphEditorTest` (`tests/editor`).
