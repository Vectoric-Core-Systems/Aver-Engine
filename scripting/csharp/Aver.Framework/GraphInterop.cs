// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// A scalar-signature interop surface Aver.Graph's IL compiler calls by reflection.

using Aver.Scene;
using Aver.Scripting;
// AP: the raw Aver.Physics surface, aliased rather than `using`d unqualified because this file
// ALREADY has a Body/Physics/Entity in scope -- Aver.Framework's own Vec3-flavoured shims (see
// Aver.Framework/Physics.cs's file comment). Forces, joints, material, motion type and layers have
// no shim here at all: Aver.Framework.Body never grew AddForce/SetFriction/SetMotionType/SetLayer
// (Physics.cs's own comment says why -- that file predates them and is not being widened), so those
// wrappers below construct an AP.Body/AP.Joint directly over the same int handle instead.
using AP = Aver.Physics;

namespace Aver.Framework;

/// <summary>Small wrappers over this assembly's own public gameplay API, reshaped for
/// System.Reflection.Emit rather than for a C# caller. GraphCompiler (Aver.Graph) emits IL that
/// calls these methods directly by reflection -- mirroring exactly how it already calls
/// Aver.Scene.Native's P/Invoke externs for getfield/setfield -- rather than duplicating a second
/// physics/input P/Invoke surface of its own. restrictedSkipVisibility:true on GraphCompiler's own
/// DynamicMethod is what lets emitted IL reach an internal method here, the same mechanism that
/// already lets it call Native's own P/Invoke externs.
///
/// WHY A SEPARATE SCALAR-SIGNATURE WRAPPER RATHER THAN CALLING Physics.Raycast DIRECTLY. Hand-emitted
/// IL cannot cheaply construct a Vec3 (a value type with a non-default constructor call) or unpack a
/// RaycastHit result (a struct with Body/Point/Normal fields, one of which -- Body -- is itself a
/// wrapping struct around an int handle) without hand-rolling several more Newobj/Ldfld sequences than
/// a plain scalar call needs. A method whose entire signature is float-in / bool,int,float-out is
/// exactly what GraphCompiler's existing "call, then read back" shape (see
/// EmitSetField/EmitExecSideEffect) already knows how to emit: push the inputs, push the ADDRESS of
/// each destination local (Ldloca), Call. No new IL pattern is needed for Raycast at all -- it is the
/// same one-native-call-many-scalars pattern GetField/SetField established, just wider.</summary>
internal static class GraphInterop
{
    /// <summary>Casts a ray and reports the first hit as five scalars instead of a RaycastHit struct.
    /// <paramref name="hit"/> is false (and the rest are 0) when nothing is hit within
    /// <paramref name="maxDistCm"/> -- mirrors RaycastHit.Hit's own "check this before reading the
    /// rest" contract, just flattened to the bool/int/float trio GraphCompiler's pin types are built
    /// from. <paramref name="entity"/> is <see cref="RaycastHit.Entity"/> -- the SCENE entity stamped
    /// on the hit body or character (see Physics.cs's <c>aver_phys_set_entity</c>), NOT the physics
    /// Body's raw handle. A hit against something no entity owns (the landscape heightfield, today)
    /// reports <paramref name="hit"/> true and <paramref name="entity"/> 0 -- check
    /// <paramref name="hit"/> first, since 0 is a real, distinguishable outcome from a miss, not an
    /// error. This used to hand back Body.Handle, which meant a graph could learn THAT it hit
    /// something but never WHAT -- see aver/physics/physics_abi.h's "Entity association" section for
    /// the native side of this fix.</summary>
    internal static void RaycastForGraph(
        float originX, float originY, float originZ,
        float dirX, float dirY, float dirZ,
        float maxDistCm,
        out bool hit, out int entity, out float pointX, out float pointY, out float pointZ)
    {
        RaycastHit result = Physics.Raycast(new Vec3(originX, originY, originZ), new Vec3(dirX, dirY, dirZ), maxDistCm);
        hit = result.Hit;
        entity = result.Entity.Handle;
        pointX = result.Point.X;
        pointY = result.Point.Y;
        pointZ = result.Point.Z;
    }

    /// <summary>Reads a Vec3-KIND scene field as three scalars -- the same scalar-in/scalar-out
    /// reshaping RaycastForGraph applies to Physics.Raycast, applied here to
    /// EntityScene.GetVec3/SetVec3's underlying P/Invoke pair (Native.cs's aver_scene_get_vec/
    /// set_vec) so GraphCompiler's hand-emitted IL never has to construct or index a float[] on the
    /// stack. <paramref name="fieldId"/> is the dense field id GraphCompiler already resolved (and
    /// kind-checked against FieldKind.Vec3) at COMPILE time via field=/_fieldResolver -- the same
    /// baked-constant convention GetField/SetField's own fieldId already uses -- NOT a qualified name
    /// string, so this needs no SceneIds lookup of its own.
    ///
    /// <paramref name="x"/>/<paramref name="y"/>/<paramref name="z"/> are 0,0,0 on any rejection
    /// (unknown entity, absent component, or -- see the arity guard below -- a field id that is not
    /// actually arity-3). Mirrors aver_scene_get_f32's own "0f on any rejection" contract rather than
    /// inventing a "found" pin GetField itself does not have.
    ///
    /// THE ARITY GUARD IS NOT REDUNDANT WITH GraphCompiler's COMPILE-TIME KIND CHECK, even though the
    /// DEFAULT field resolver and this method ultimately query the same live field table and can never
    /// actually disagree with each other. GraphCompiler accepts an INJECTABLE FieldResolver precisely
    /// so it can be unit-tested without a live native scene (every kind-check test in this codebase's
    /// own test suite passes a fake one) -- a fake resolver that claims kind=Vec3 for a fieldId this
    /// method's REAL native scene disagrees about is exactly the kind of divergence a compile-time-only
    /// check cannot catch. aver_scene_get_vec copies `arity` floats into whatever buffer it is given
    /// (SceneAbi.cpp) -- called against a Quat (arity 4) field with this method's fixed 3-float
    /// scratch, that is a buffer overrun, not a wrong number silently returned. Checking arity==3 here
    /// too is the same belt-and-suspenders EntityScene.GetVec3/SetVec3 already apply, for the identical
    /// reason (see that method's own comment).</summary>
    internal static void GetFieldVecForGraph(int entity, int fieldId, out float x, out float y, out float z)
    {
        x = y = z = 0f;
        if (SceneNative.aver_scene_field_arity(fieldId) != 3) return;
        float[] o = Entity.Scratch3;
        if (SceneNative.aver_scene_get_vec(entity, fieldId, o) == 0) return;
        x = o[0]; y = o[1]; z = o[2];
    }

    /// <summary>Writes a Vec3-KIND scene field from three scalars -- the write half of
    /// GetFieldVecForGraph, see that method's comment for the fieldId/arity-guard reasoning shared by
    /// both. Returns aver_scene_set_vec's own real return code (true on success, false on ANY
    /// rejection -- unknown entity, wrong arity, read-only field, or missing component) so it can reach
    /// a "success" pin exactly the way EmitExecSideEffect already surfaces SetField's own return
    /// code.</summary>
    internal static bool SetFieldVecForGraph(int entity, int fieldId, float x, float y, float z)
    {
        if (SceneNative.aver_scene_field_arity(fieldId) != 3) return false;
        float[] s = Entity.Scratch3;
        s[0] = x; s[1] = y; s[2] = z;
        return SceneNative.aver_scene_set_vec(entity, fieldId, s) != 0;
    }

    /// <summary>Spawns a registered class by NAME at a position, unrotated and unscaled -- the
    /// scalar-in/scalar-out reshaping RaycastForGraph/GetFieldVecForGraph apply to their own native
    /// surfaces, applied here to <see cref="Actors"/>.Spawn(ActorClass, Vec3), which already has exactly
    /// this shape and needs no new native ABI (see FrameworkAbi.cpp's aver_fw_spawn, which Actors.Spawn
    /// already calls). Hand-emitted IL cannot cheaply construct the ActorClass/Vec3 value types that
    /// overload wants on the stack, so this takes a class NAME and three floats instead and does the
    /// construction here, in ordinary C#.
    ///
    /// RESOLVED AT RUNTIME, DELIBERATELY NOT BAKED TO A CLASS HANDLE AT COMPILE TIME THE WAY field=
    /// bakes a field id (RequireVec3Field / GetField's own fieldId lookup). GetField/SetField can bake
    /// fieldId at compile time because the scene's field table is a fixed, engine-global set that exists
    /// before any project loads -- FieldResolver's own doc comment says so explicitly. A project's actor
    /// classes are NOT: they are declared by that project's own Scripts.dll, at a point in each host's
    /// boot order (LoadScripts, then graph compilation -- confirmed by reading GameApp.cpp's onInit and
    /// SandboxApp.cpp's scripts_.init, both of which run LoadScripts before any graph is ever compiled)
    /// that happens to hold today, but is not something this method or GraphCompiler asserts or depends
    /// on. Baking the resolved handle into IL at compile time would tie GraphCompiler to that ordering
    /// holding FOREVER across every current and future host, and would need a second injectable
    /// resolver abstraction (mirroring FieldResolver) purely to keep this unit-testable without a live
    /// scripting host -- exactly the complication the README's own "only a compiler case, no new native
    /// ABI" framing argues against. Resolving here, at INVOCATION time, needs neither: by the time a
    /// compiled graph's delegate actually runs (a tick, an event), every class the project declares has
    /// always already been declared, with no host-ordering assumption baked into the compiler at all.
    ///
    /// Returns entity 0 -- the same "silently did nothing" convention aver_fw_spawn's own spawnActor
    /// already uses for an invalid class handle (FrameworkAbi.cpp: `ClassRecord* r = rec(c); if (!r)
    /// return 0;`) -- when className names no registered class, rather than throwing: a graph author's
    /// typo in a class= attribute should read as "nothing spawned" on the entity output pin, the same
    /// way GetField reads as 0f against an unknown entity rather than throwing mid-tick.</summary>
    internal static int SpawnForGraph(string className, float x, float y, float z)
    {
        ActorClass c = ActorClass.Find(className);
        if (!c.IsValid) return 0;
        return Actors.Spawn(c, new Vec3(x, y, z)).Handle;
    }

    /// <summary>Reads this frame's mouse delta and wheel as three scalars from ONE call to
    /// aver_fw_input_mouse -- the LOOK half of continuous input (the other half is
    /// MoveAxisForGraph, below). Deliberately does NOT go through Input.MouseDeltaX/MouseDeltaY/
    /// MouseWheel: each of those three PROPERTIES independently calls aver_fw_input_mouse in its own
    /// getter (Input.cs), so wiring GraphCompiler's emitted IL straight at them -- one call per pin,
    /// as InputKey's own single-scalar shape would naturally suggest -- would cost three native calls
    /// for one frame's worth of state instead of one. This wrapper reads the packed {dx,dy,wheel}
    /// buffer once (reusing Entity.Scratch3, exactly like GetFieldVecForGraph does for
    /// aver_scene_get_vec) and unpacks all three, mirroring GetFieldVecForGraph's own shape one level
    /// up: a float[]-taking P/Invoke reshaped into scalar out-params so hand-emitted IL never has to
    /// allocate or index an array on the stack -- the same "cannot cheaply construct/unpack" problem
    /// this file's own header comment names for Raycast/Vec3.
    ///
    /// CALLED AT MOST ONCE PER EXEC VISIT ON THE PUSH COMPILER, NOT PER PIN -- and this is a
    /// DELIBERATE DEPARTURE from GetFieldVec3's own "no _execLocals caching, a Vec3 read is a
    /// same-cost sibling of GetField's single-float memcpy" precedent (see
    /// GraphCompiler.EmitPullGetFieldVec3's comment). aver_fw_input_mouse IS exactly that same cost
    /// class -- FrameworkAbi.cpp's implementation is a three-float struct-field copy, nothing more --
    /// so cost alone would argue for GetFieldVec3's uncached shape here too. The reason this method is
    /// instead wired through Raycast's exec-cached shape (see GraphCompiler.EmitExecMouseDelta /
    /// IsExecCapableMouseDeltaType) is a DIFFERENT, EXPLICIT requirement this slice was built against:
    /// one frame's mouse state must cost exactly one native call regardless of how many of
    /// deltaX/deltaY/wheel a graph reads back, not "cheap enough that repeating it doesn't matter."
    /// Idempotent within a frame either way (aver_fw_input_new_frame() only mutates the underlying
    /// state once per real engine frame, before any graph runs), so nothing here is UNSAFE to call
    /// more than once -- only wasteful, which is exactly what the exec-cached shape avoids.</summary>
    internal static void MouseDeltaForGraph(out float deltaX, out float deltaY, out float wheel)
    {
        float[] o = Entity.Scratch3;
        Fw.aver_fw_input_mouse(o);
        deltaX = o[0]; deltaY = o[1]; wheel = o[2];
    }

    /// <summary>Reads this frame's WASD/arrow movement axis as two scalars -- the MOVE half of
    /// continuous input, see MouseDeltaForGraph's own comment for the LOOK half and for why both are
    /// wired through the exec-cached (Raycast-shaped) PUSH-compiler path rather than GetFieldVec3's
    /// uncached one. Unpacks Input.MoveAxis's X (forward) and Y (right) components; Z is NOT a
    /// parameter here -- Input.MoveAxis's own doc says Z is hardcoded 0 always (Input.cs), so a third
    /// out-param that could only ever read a compile-time-known constant would add noise, not
    /// information, to every graph that uses this node.
    ///
    /// Unlike MouseDeltaForGraph's single P/Invoke, Input.MoveAxis itself makes roughly eight separate
    /// aver_fw_input_key calls (one GetKey per WASD/arrow key) to assemble its Vec3 -- an existing cost
    /// inherent to MoveAxis's own definition, unchanged by wrapping it for a graph. What THIS wrapper
    /// guarantees is that those eight calls happen at most ONCE per exec visit (one call to THIS
    /// method, not one per output pin) rather than doubling to ~16 if a graph reads forward AND right
    /// independently -- the same "one wrapper call, however many pins" property MouseDeltaForGraph
    /// gives its own single native call.</summary>
    internal static void MoveAxisForGraph(out float forward, out float right)
    {
        Vec3 v = Input.MoveAxis;
        forward = v.X; right = v.Y;
    }

    /// <summary>SetMesh's own surface: sets the drawn mesh by asset path, adding a mesh renderer if the
    /// entity has none. NOT a new native ABI and NOT a generalised I64-capable SetField -- this is a
    /// one-line forward to <see cref="Entity"/>.SetMesh (EntityScene.cs), which already composes
    /// EnsureMeshRenderer() + Assets.ObjectIdOf(path) + SetInt64 the exact way the C# side of a
    /// first-person controller would. Constructing an <see cref="Entity"/> from a raw handle needs its
    /// `internal` constructor (Entity.cs), which this method can call freely -- it lives in the SAME
    /// assembly, unlike RaycastForGraph/SpawnForGraph's own reshaping of a DIFFERENT surface
    /// (Aver.Scene.Native / Physics) into scalars. Assets.ObjectIdOf is a PURE LOCAL HASH (FNV1a64 over
    /// the path's UTF-8 bytes, Aver.Scene/Native.cs) -- no native call, no I/O, no lookup table -- so the
    /// only native call this method's IL reaches at all is SetInt64's own aver_scene_set_i64. Returns
    /// SetInt64's real return code (false on an unknown entity or a missing component EnsureMeshRenderer
    /// somehow failed to add), mirroring GetFieldVecForGraph/SetFieldVecForGraph's own "surface the real
    /// return code" convention rather than SpawnForGraph's "0 means nothing happened" one -- this method
    /// already returns a bool, so there is no analogous "invalid sentinel" to invent.</summary>
    internal static bool SetMeshForGraph(int entity, string meshPath) => new Entity(entity).SetMesh(meshPath);

    /// <summary>SetMaterial's own surface: sets the material by name, adding a mesh renderer if the
    /// entity has none. Mirrors SetMeshForGraph immediately above exactly -- see that method's comment,
    /// which applies unchanged here (Entity.SetMaterial composes EnsureMeshRenderer() +
    /// aver_scene_material(0, name) + SetInt, EntityScene.cs).</summary>
    internal static bool SetMaterialForGraph(int entity, string materialName) => new Entity(entity).SetMaterial(materialName);

    /// <summary>AttachToSocket's own surface: hangs <paramref name="entity"/> on a named socket of
    /// <paramref name="parent"/>'s rig. Wraps <see cref="Entity.AttachToSocket"/>, which is SetParent
    /// plus a component field -- the same "needs Entity's internal constructor, which only
    /// Aver.Framework code can call" reason SetMeshForGraph above has for existing at all.
    ///
    /// IDEMPOTENT, so this node is allowed in the PULL compiler alongside SetMesh/SetParent rather
    /// than being push-only like Spawn: attaching to the same parent and socket twice is the same
    /// state, not two attachments.
    ///
    /// TRUE MEANS THE FIELDS WERE WRITTEN, not that the socket resolved -- the parent's rig may not
    /// even be loaded yet, and a name that matches nothing leaves the entity where it is. See
    /// Entity.AttachToSocket's own comment for why that is the chosen failure.</summary>
    internal static bool AttachToSocketForGraph(int entity, int parent, string socket) =>
        new Entity(entity).AttachToSocket(new Entity(parent), socket);

    /// <summary>GetAnimCurve's own surface: the value of a named curve on the clip
    /// <paramref name="entity"/> is playing, or 0 when there is no such curve.
    ///
    /// A SINGLE FLOAT, NOT A (value, found) PAIR, and that is a genuine loss stated rather than
    /// hidden: a graph node returns through pins, PinType has Float and Bool, and a node CAN carry
    /// both -- but the emitter would then need two locals and a second output for a distinction a
    /// graph author almost never branches on. The C# surface keeps TryGetAnimationCurve for the
    /// callers that do care.</summary>
    internal static float GetAnimCurveForGraph(int entity, string curve) =>
        new Entity(entity).GetAnimationCurve(curve, 0.0f);

    /// <summary>SetSkeleton's own surface: binds a skeleton asset by path, adding a CSkeletalMesh
    /// component if the entity has none. Mirrors SetMeshForGraph's own reasoning exactly -- see that
    /// method's comment -- wrapping <see cref="Entity.SetSkeleton"/> (Animation.cs) instead, which
    /// composes AddComponent(Component.SkeletalMesh) + Assets.ObjectIdOf(path) + SetInt64 the same
    /// three-step shape Entity.SetMesh does.</summary>
    internal static bool SetSkeletonForGraph(int entity, string skeletonAsset) =>
        new Entity(entity).SetSkeleton(skeletonAsset);

    /// <summary>PlayAnimation's own surface: plays a clip from the start, adding a CAnimator component
    /// if the entity has none. Wraps <see cref="Entity.PlayAnimation"/> (Animation.cs), which -- unlike
    /// every other GraphInterop wrapper in this animation group -- writes THREE fields (flags, time,
    /// clip), not one: loop folds into CAnimator.flags's AnimatorOnce bit, and the playhead is reset to
    /// 0 so a re-trigger genuinely restarts the clip rather than continuing wherever the last one left
    /// off. Still idempotent in the sense this node family requires -- calling it twice with the same
    /// arguments leaves the entity in the same state, not two overlapping plays -- because there is
    /// only one CAnimator per entity for it to write into.</summary>
    internal static bool PlayAnimationForGraph(int entity, string clipAsset, bool loop) =>
        new Entity(entity).PlayAnimation(clipAsset, loop);

    /// <summary>SetControlRig's own surface: binds an .ocrig by path so the entity's sampled pose is
    /// modified before skinning, adding a CControlRig component if it has none. Wraps
    /// <see cref="Entity.SetControlRig"/> (Animation.cs).
    ///
    /// <para>Unlike its two siblings above, the component this attaches is registered at RUNTIME, so
    /// this returns false in a host that never registered CControlRig -- and false, not a throw, is
    /// the right answer: a graph authored against a rig is not broken content when it runs somewhere
    /// the rig system is absent, it simply does not get its rig.</para>
    ///
    /// <para>Ordering with SetSkeleton is a real constraint and it is the graph author's to satisfy:
    /// a rig has nothing to modify until the entity has a skeleton, because the pose it edits is the
    /// one AnimSystem samples for a skeleton. This node does not enforce that -- attaching in the
    /// other order is harmless and self-corrects the moment a skeleton arrives, since the rig is
    /// applied per tick from the component, not once at attach time.</para></summary>
    internal static bool SetControlRigForGraph(int entity, string rigAsset, float weight) =>
        new Entity(entity).SetControlRig(rigAsset, weight);

    /// <summary>CharacterMove's own surface: the last Blueprint-parity node, one coarse exec call
    /// wrapping <see cref="AverCharacter"/>.DriveFromGraph -- itself a one-line forward to the
    /// existing <c>protected</c> Drive(dt, moveAxis, yawDeltaDeg, pitchDeltaDeg), which owns the
    /// pitch clamp, the view mode and the capsule. NOT a reimplementation of Drive, and NOT a cast or
    /// a reflection hack around its protection -- see DriveFromGraph's own comment for why a seam
    /// method is required rather than either.
    ///
    /// <paramref name="entity"/> is resolved through <see cref="Actors"/>.Get, the SAME runtime
    /// entity-to-instance lookup a C# caller would use (Actors.cs), not a baked handle -- mirroring
    /// SpawnForGraph's own "resolved at invocation time, not compile time" reasoning, just for a
    /// lookup rather than a class name.
    ///
    /// FAILS VISIBLY, NEVER SILENTLY, on either of the two ways this can go wrong -- a real `false`
    /// on the return value (the node's "success" pin) AND a Log.Warn line, so a controller bound to
    /// the wrong entity is distinguishable from a broken one, exactly as the task requires:
    ///   * no live actor is bound to <paramref name="entity"/> at all (dead entity, never spawned as
    ///     a managed actor, or Actors.Resolver itself not installed in this host) -- Actors.Get
    ///     returns null;
    ///   * a live actor IS bound, but it is not an <see cref="AverCharacter"/> (some other actor
    ///     class entirely) -- the pattern match below fails.
    /// These are reported as two DIFFERENT messages (not collapsed into one generic "can't move"),
    /// because they are different authoring mistakes: the first is usually a bad entity id reaching
    /// the graph, the second is usually a class field/level pointing this node at the wrong actor.
    ///
    /// Returns true, with no further reporting, on success -- mirrors
    /// SetFieldVecForGraph/SetMeshForGraph's own "surface the real return code" convention rather
    /// than SpawnForGraph's "0 means nothing happened" sentinel, since this method already returns a
    /// bool with nothing left to invent.</summary>
    internal static bool CharacterMoveForGraph(int entity, float dt, float forward, float right, float yawDeltaDeg, float pitchDeltaDeg)
    {
        Entity e = new Entity(entity);
        AverActor? actor = Actors.Get(e);
        if (actor is not AverCharacter character)
        {
            Log.Warn(actor is null
                ? $"[Graph] CharacterMove: entity {entity} cannot be driven -- no live actor is bound to it"
                : $"[Graph] CharacterMove: entity {entity} cannot be driven -- its actor is a {actor.GetType().Name}, not an AverCharacter");
            return false;
        }
        character.DriveFromGraph(dt, new Vec3(forward, right, 0f), yawDeltaDeg, pitchDeltaDeg);
        return true;
    }

    /// <summary>GetForward's own surface: where a character is LOOKING, and where its eyes are.
    ///
    /// WHY THIS EXISTS AT ALL. <see cref="AverCharacter"/> keeps <c>_yaw</c>/<c>_pitch</c> private and
    /// publishes the aim only as <see cref="AverCharacter.LookDirection"/>; the rotation it writes to
    /// the scene is a Quat, and the graph vocabulary's two readers cannot see it -- GetField takes F32
    /// fields only and GetFieldVec3 requires FieldKindVec3 specifically (see RequireVec3Field). So a
    /// graph could drive a character's look through CharacterMove and then had no way whatsoever to ask
    /// which way that look ended up pointing. Rebuilding it graph-side from accumulated mouse deltas
    /// with Sin/Cos was possible but wrong: it would duplicate state Character.cs already owns,
    /// including the pitch CLAMP applied in Drive (PitchMin/PitchMax), and any drift between the two
    /// copies shows up as a shot that does not go where the camera points.
    ///
    /// BOTH HALVES, ONE CALL, because a direction alone cannot build a ray. The eye position is the
    /// origin a first-person shot must start from -- and specifically must start ABOVE the shooter's
    /// own capsule, or the very first thing the ray hits is the character firing it. test-content's
    /// AN_Playable sample had to hand-compute a constant origin at z=290 for exactly that reason;
    /// <see cref="AverCharacter.EyePosition"/> is that number, correct for any character at any
    /// position, and returning it beside the direction is what lets a graph wire Raycast without
    /// arithmetic.
    ///
    /// Fails the same VISIBLE way CharacterMoveForGraph does, with the same two distinguishable
    /// messages, and leaves every out-parameter at zero. A zero direction makes Raycast a no-op rather
    /// than firing somewhere arbitrary, which is the failure a graph author can actually see.</summary>
    internal static bool LookDirectionForGraph(int entity,
                                              out float dirX, out float dirY, out float dirZ,
                                              out float eyeX, out float eyeY, out float eyeZ)
    {
        dirX = dirY = dirZ = 0f;
        eyeX = eyeY = eyeZ = 0f;

        Entity e = new Entity(entity);
        AverActor? actor = Actors.Get(e);
        if (actor is not AverCharacter character)
        {
            Log.Warn(actor is null
                ? $"[Graph] GetForward: entity {entity} has no look direction -- no live actor is bound to it"
                : $"[Graph] GetForward: entity {entity} has no look direction -- its actor is a {actor.GetType().Name}, not an AverCharacter");
            return false;
        }

        Vec3 dir = character.LookDirection;
        Vec3 eye = character.EyePosition;
        dirX = dir.X; dirY = dir.Y; dirZ = dir.Z;
        eyeX = eye.X; eyeY = eye.Y; eyeZ = eye.Z;
        return true;
    }

    /// <summary>GetViewEntity's own surface: the CAMERA node a character looks through.
    ///
    /// WHY A GRAPH NEEDS THIS. Anything that should sit still relative to the CAMERA rather than the
    /// character -- a first-person weapon above all -- has to be parented to the view node, not to the
    /// pawn. Parent a gun to the character and it stays put while the camera pitches around it; parent
    /// it to the view and it moves with the eye, which is what a viewmodel is.
    ///
    /// <see cref="AverCharacter"/> creates that node in EnsureView() and publishes it only as
    /// <see cref="AverCharacter.View"/>. A graph had `SetParent` and `SetMesh` and no way to NAME the
    /// thing to parent to -- the same shape of gap as LookDirection before GetForward: state the
    /// character already owns that no node could read.
    ///
    /// Returns 0 with false when there is no character, or when its view node does not exist yet --
    /// EnsureView is lazy, so a graph asking on the very first OnStart before CharacterMove has run
    /// legitimately gets nothing. That is a "try again next tick", not an error, which is why it fails
    /// quietly here rather than warning every frame the way a wrong-actor-type would.</summary>
    internal static bool ViewEntityForGraph(int entity, out int view)
    {
        view = 0;

        Entity e = new Entity(entity);
        AverActor? actor = Actors.Get(e);
        if (actor is not AverCharacter character)
        {
            Log.Warn(actor is null
                ? $"[Graph] GetViewEntity: entity {entity} has no view -- no live actor is bound to it"
                : $"[Graph] GetViewEntity: entity {entity} has no view -- its actor is a {actor.GetType().Name}, not an AverCharacter");
            return false;
        }

        Entity v = character.View;
        if (!v.IsAlive) return false;   // lazy: not built yet, ask again next tick
        view = v.Handle;
        return true;
    }

    /// <summary>Jump's own surface: one call into <see cref="AverCharacter.Jump"/>, which refuses in
    /// mid-air by returning false.
    ///
    /// THE GROUNDED CHECK IS NOT THIS NODE'S TO MAKE. Jump() already asks the physics character
    /// whether it is standing on something (Phys.aver_phys_character_grounded) and declines if not, so
    /// a graph wiring this straight to a key gets single jumps and no flight for free. Re-testing it
    /// here would mean a second answer to the same question that could disagree with the first.
    ///
    /// THE RETURN IS THE INTERESTING PART, and it is why this reports `jumped` rather than nothing: a
    /// graph that wants a jump SOUND, an animation, or a counter needs to know whether the jump
    /// actually happened, and "the key was pressed" is not that. False here means airborne, which is
    /// ordinary and frequent -- so unlike the wrong-actor case below it is not logged at all. A warning
    /// every frame the player holds the jump key would be noise, not diagnosis.</summary>
    /// <summary>Print's surface: one log line, labelled with the graph node's own id.
    ///
    /// THE ONLY WAY A GRAPH COULD OBSERVE ITSELF BEFORE THIS was to route a value all the way to
    /// an OUT record and read it back off GraphHost's per-tick log -- which works for exactly one
    /// value per graph, has to reach the graph's own output to exist at all, and says nothing about
    /// which branch was taken or whether a chain ran. A Blueprint author reaches for Print String
    /// before anything else, and this vocabulary did not have it.
    ///
    /// FLOAT, NOT A STRING, because there is no string PIN TYPE -- PinType is Float, Int, Bool and
    /// Exec, and inventing one for this would touch the parser, the writer, the editor's pin
    /// colours and Validate's type check. A number and a name covers what a graph is usually asking.</summary>
    internal static void PrintForGraph(string label, float value)
    {
        Log.Info($"[Graph] {label} = {value}");
    }

    /// <summary>Print for an INT pin, and it is not a convenience -- it is a correctness fix.
    /// A float32 has 24 mantissa bits, so above 16777216 it can only hold EVEN integers, and this
    /// engine's ENTITY HANDLES START AT 16777216. Routing a handle through IntToFloat to reach
    /// Print silently rounds it to its neighbour, which reads exactly like the engine returning the
    /// wrong entity. It cost an hour of chasing a framework bug that was never there.</summary>
    internal static void PrintIntForGraph(string label, int value)
    {
        Log.Info($"[Graph] {label} = {value}");
    }

    /// <summary>PrintString: an authored message, with the node id kept as a prefix so two nodes
    /// carrying the same text are still tellable apart. The one print that needs nothing wired but
    /// exec, which is what makes it the node for "did control flow reach here".
    ///
    /// The "[Graph] " prefix is load-bearing beyond tidiness: the editor's on-screen print overlay
    /// filters the engine log on exactly that prefix (SandboxApp::logSink), so a line without it is
    /// written to the log and never appears in the viewport.</summary>
    internal static void PrintStringForGraph(string label, string text)
    {
        Log.Info($"[Graph] {label}: {text}");
    }

    // ---- node-hit recording, for the editor's execution highlighting -----------------------------
    //
    // Every exec node calls this as it runs, so the graph editor can show which nodes are actually
    // executing rather than leaving an author to infer it from prints. Visual scripting had no way at
    // all to see control flow: you could print a value, and nothing showed you WHICH branch ran.
    //
    // OFF UNLESS THE EDITOR ASKS. This sits on the hot path of every exec node of every live graph
    // instance, so with recording off it must cost a static bool test and nothing else -- no
    // allocation, no dictionary probe, no time read. A packaged game never turns it on.
    //
    // NOT THE LOG CHANNEL, though that already reaches the editor. PrintStringForGraph is a string
    // interpolation plus a Log.Info under the core log mutex, then a cross-thread substring filter on
    // the other side; paying that per exec node per frame per instance would make the profiler part
    // of what it profiles.
    //
    // KEYED BY (GRAPH NAME, NODE ID), BOTH COMPILE-TIME CONSTANTS. Not by entity: the compiled
    // method's arguments come from the graph's declared PARAM list, so there is no "entity is always
    // argument 0" to lean on, and a graph that declares no entity PARAM has none to report. Keying by
    // graph name is also what the editor actually wants -- its canvas shows a CLASS, and any instance
    // running a node should light that node.
    //
    // LAST-HIT TIME, NEVER A COUNTER. A diamond in the exec graph (two branch arms rejoining) makes
    // the shared node's IL be emitted TWICE at compile time, so a counter would over-report by
    // construction. "When did this last run" is both the honest measure and the one a fading
    // highlight needs.
    private static bool s_recordHits;
    private static readonly Dictionary<string, double> s_nodeHits = new();
    private static readonly System.Diagnostics.Stopwatch s_hitClock = System.Diagnostics.Stopwatch.StartNew();

    internal static void RecordNodeHitForGraph(string graphName, string nodeId)
    {
        if (!s_recordHits) return;
        lock (s_nodeHits) s_nodeHits[graphName + "\0" + nodeId] = s_hitClock.Elapsed.TotalSeconds;
    }

    /// <summary>Turns recording on or off. Called from the bridge when a graph editor tab opens or
    /// closes, so the cost exists only while somebody is looking.</summary>
    internal static void SetNodeHitRecording(bool on)
    {
        s_recordHits = on;
        if (!on) lock (s_nodeHits) s_nodeHits.Clear();
    }

    /// <summary>Node ids of `graphName` hit within `maxAgeSeconds`, with their ages, as
    /// "id:age;id:age". Ages rather than timestamps because the two sides do not share a clock.</summary>
    internal static string CollectNodeHits(string graphName, double maxAgeSeconds)
    {
        var sb = new System.Text.StringBuilder();
        string prefix = graphName + "\0";
        lock (s_nodeHits)
        {
            double now = s_hitClock.Elapsed.TotalSeconds;
            foreach (var kv in s_nodeHits)
            {
                if (!kv.Key.StartsWith(prefix, StringComparison.Ordinal)) continue;
                double age = now - kv.Value;
                if (age > maxAgeSeconds) continue;
                if (sb.Length > 0) sb.Append(';');
                sb.Append(kv.Key.AsSpan(prefix.Length)).Append(':').Append(age.ToString("0.000"));
            }
        }
        return sb.ToString();
    }

    /// <summary>The character this entity is, or null with one warning line. Every character node
    /// below funnels through here so they all fail the same way and say the same thing -- the shape
    /// JumpForGraph and GetViewEntity already established.</summary>
    private static AverCharacter? CharacterFor(int entity, string node)
    {
        AverActor? actor = Actors.Get(new Entity(entity));
        if (actor is AverCharacter c) return c;
        Log.Warn(actor is null
            ? $"[Graph] {node}: entity {entity} has no live actor bound to it"
            : $"[Graph] {node}: entity {entity} is a {actor.GetType().Name}, not an AverCharacter");
        return null;
    }

    /// <summary>AverCharacter.Velocity, in cm/s. False (and zeroes) when there is no character or it
    /// is not simulated -- an unsimulated character HAS no velocity, which is different from having
    /// one of zero, and the success pin is how a graph can tell those apart.</summary>
    // ---- tags and visibility ---------------------------------------------------------------------
    //
    // A TAG IS A BITMASK, NOT A STRING, which is why this family needed no new format machinery at
    // all. Every other string-shaped API (class=, name=, field=, var=) had to invent a compile-time
    // NODE attribute because PinType has no string; Entity.Tags is a uint over CTags.bits, so an
    // ordinary int pin carries it and a graph can compute one -- combine two masks with a bitwise Or
    // node, test several at once, or read a mask out of a VAR.
    //
    // int RATHER THAN uint on the pin, because PinType has no unsigned type and inventing one to
    // carry a bit pattern would be a new pin type for a reinterpretation. `unchecked((uint))` is the
    // whole of the conversion, and it is exactly what Entity.Tags already does in the other
    // direction (EntityScene.cs). A mask with the top bit set arrives as a negative int and works.

    /// <summary>Shows or hides an entity. False when the handle names nothing alive.</summary>
    internal static bool SetVisibleForGraph(int entity, bool visible)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        return e.SetVisible(visible);
    }

    /// <summary>ORs `mask` into the entity's tags.</summary>
    internal static bool AddTagForGraph(int entity, int mask)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        return e.AddTag(unchecked((uint)mask));
    }

    /// <summary>Clears every bit of `mask` from the entity's tags.</summary>
    internal static bool RemoveTagForGraph(int entity, int mask)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        return e.RemoveTag(unchecked((uint)mask));
    }

    /// <summary>True when EVERY bit of `mask` is set -- Entity.HasTag's own all-of rule, not any-of,
    /// so a graph testing two tags at once gets the same answer C# does. A zero mask is false.</summary>
    internal static bool HasTagForGraph(int entity, int mask)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        return e.HasTag(unchecked((uint)mask));
    }

    /// <summary>The whole tag mask, so a graph can test bits the node vocabulary has no operator
    /// for, stash it in a VAR, or compare two entities' tags directly. 0 for a dead handle.</summary>
    internal static int TagsForGraph(int entity)
    {
        Entity e = new Entity(entity);
        return e.IsAlive ? unchecked((int)e.Tags) : 0;
    }

    internal static bool VelocityForGraph(int entity, out float x, out float y, out float z)
    {
        x = y = z = 0.0f;
        AverCharacter? c = CharacterFor(entity, "GetVelocity");
        if (c is null || !c.IsSimulated) return false;
        Vec3 v = c.Velocity;
        x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    /// <summary>Sets the character velocity directly. This is a TELEPORT OF MOMENTUM, not a force:
    /// it replaces whatever the physics character was doing, which is what a graph asking for it
    /// almost always means (a launch pad, a dash, a stop).</summary>
    internal static bool SetVelocityForGraph(int entity, float x, float y, float z)
    {
        AverCharacter? c = CharacterFor(entity, "SetVelocity");
        if (c is null || !c.IsSimulated) return false;
        c.Velocity = new Vec3(x, y, z);
        return true;
    }

    /// <summary>AverCharacter.IsGrounded. False rather than an error when the entity is not a
    /// character at all: a graph asking "am I on the ground" about a crate has its answer.</summary>
    internal static bool IsGroundedForGraph(int entity)
    {
        AverActor? actor = Actors.Get(new Entity(entity));
        return actor is AverCharacter c && c.IsGrounded;
    }

    /// <summary>AverCharacter.Teleport: moves the character AND its capsule to a feet position and
    /// clears velocity. Setting the transform alone leaves the physics capsule behind, and the
    /// character snaps back on the next step -- which is exactly the bug a graph would write if it
    /// reached for SetFieldVec3 on CLocal.position instead of this.</summary>
    internal static bool TeleportForGraph(int entity, float x, float y, float z)
    {
        AverCharacter? c = CharacterFor(entity, "Teleport");
        if (c is null) return false;
        c.Teleport(new Vec3(x, y, z));
        return true;
    }

    /// <summary>Game.GetPlayerPawn / GetPlayerController, as raw handles. 0 when there is none, which
    /// is what Entity.IsValid tests, so a graph can Branch on it without a second pin.</summary>
    internal static int PlayerPawnForGraph(int index) => Game.GetPlayerPawn(index).Handle;
    internal static int PlayerControllerForGraph(int index) => Game.GetPlayerController(index).Handle;
    internal static int GameModeForGraph() => Game.Mode.Handle;

    /// <summary>Game.IsPlaying -- true only in a running session, false in the editor. The one thing
    /// a graph needs to know before it does anything irreversible.</summary>
    internal static bool IsPlayingForGraph() => Game.IsPlaying;

    /// <summary>Game.SaveGame / LoadGame, unchanged -- see their own doc comments for the atomic
    /// write and the "destroy everything, then restore" load. Nothing wrapped here needs an
    /// Entity's internal constructor the way SetMesh/SetMaterial do; this is a direct forward,
    /// exactly like IsPlayingForGraph just above.
    ///
    /// LoadGameForGraph CAN DESTROY THE ENTITY CALLING IT. If a "loadgame" node fires from inside
    /// this very entity's own graph, the world tear-down Game.LoadGame documents runs OnEndPlay on
    /// that entity mid-exec, same as it would on any other. Not a new hazard this wrapper adds --
    /// Game.LoadGame already carries it, node or no node -- and Blueprint's own Load Game node has
    /// the identical footgun, so nothing here tries to guard against it.</summary>
    internal static bool SaveGameForGraph(string path) => Game.SaveGame(path);
    internal static bool LoadGameForGraph(string path) => Game.LoadGame(path);

    /// <summary>AverPlayerController.Possess / Unpossess. Possession is what makes the game camera
    /// follow a pawn at all (see the FirstPerson template README), so a graph that spawns a
    /// character and wants it controlled needs this and nothing else.</summary>
    internal static bool PossessForGraph(int controller, int pawn)
    {
        if (Actors.Get(new Entity(controller)) is not AverPlayerController pc)
        {
            Log.Warn($"[Graph] Possess: entity {controller} is not an AverPlayerController");
            return false;
        }
        return pc.Possess(new Entity(pawn));
    }

    internal static bool UnpossessForGraph(int controller)
    {
        if (Actors.Get(new Entity(controller)) is not AverPlayerController pc)
        {
            Log.Warn($"[Graph] Unpossess: entity {controller} is not an AverPlayerController");
            return false;
        }
        return pc.Unpossess();
    }

    /// <summary>Entity.WorldPosition, and the reason it is a node when GetFieldVec3 on
    /// CLocal.position already exists: LOCAL IS NOT WORLD. A gun parented to a camera has a local
    /// position of a few centimetres forever, and a graph asking where it actually is in the level
    /// -- to measure a distance, aim something, place an effect -- wants this one.</summary>
    internal static bool WorldPositionForGraph(int entity, out float x, out float y, out float z)
    {
        Entity e = new Entity(entity);
        x = y = z = 0.0f;
        if (!e.IsAlive) return false;
        Vec3 v = e.WorldPosition;
        x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    /// <summary>One of the entity's WORLD axes: 0 forward, 1 right, 2 up. Three nodes share this
    /// one surface because they differ only in which axis they ask for, and an axis index chosen by
    /// the emitter is cheaper than three near-identical methods.
    ///
    /// Distinct from the existing GetForward node, which reads an AverCharacter's look direction
    /// including its pitch clamp. This is the transform's own orientation and works on anything.</summary>
    internal static bool EntityAxisForGraph(int entity, int axis, out float x, out float y, out float z)
    {
        Entity e = new Entity(entity);
        x = y = z = 0.0f;
        if (!e.IsAlive) return false;
        Vec3 v = axis == 0 ? e.WorldForward : axis == 1 ? e.WorldRight : e.WorldUp;
        x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    internal static bool LocalScaleForGraph(int entity, out float x, out float y, out float z)
    {
        Entity e = new Entity(entity);
        x = y = z = 0.0f;
        if (!e.IsAlive) return false;
        Vec3 v = e.LocalScale;
        x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    // ================================================================== audio
    //
    // sound= is a PATH, and a NODE-line attribute rather than a pin, for SetName's exact reason:
    // which file to play is chosen at edit time and PinType has no String member. Load is cached
    // native-side (the same path returns the same handle without decoding again), so calling these
    // every time the node runs costs a dictionary probe, not a decode.

    /// <summary>Turns a graph's raw int pin into a <see cref="Bus"/>, saying so when it is not one.</summary>
    ///
    /// <remarks>THE ONE PLACE AN OUT-OF-RANGE BUS CAN ARRIVE. Every other caller now names a bus
    /// through the enum, but a graph pin is an integer a person typed into a node. Native
    /// <c>busOf</c> folds anything it does not recognise into Sfx and is right to -- but silently, so
    /// a Set Bus Volume node set to 7 moves the SFX slider and nothing anywhere says why. This does
    /// not change that behaviour, it just stops it being silent.</remarks>
    private static Bus BusOfPin(int bus, string node)
    {
        if (System.Enum.IsDefined(typeof(Bus), bus)) return (Bus)bus;
        Aver.Scripting.Log.Warn($"[{node}] bus {bus} is not one of Sfx(0)/Music(1)/Voice(2)/Ui(3); it plays on Sfx.");
        return Bus.Sfx;
    }

    /// <summary>PlaySound's own surface: load-by-path then play flat, as one scalar call. Returns
    /// the VOICE handle so a graph can stop or steer it later, and 0 when there is no audio device
    /// at all -- which is a supported configuration, not an error (see Audio's own comment).</summary>
    internal static bool PlaySoundForGraph(string path, float volume, float pitch, bool looping, int bus,
                                           out int voice)
    {
        Voice v = Audio.PlayFile(path, volume, pitch, looping, BusOfPin(bus, "PlaySound"));
        voice = v.Handle;
        return v.IsValid;
    }

    /// <summary>PlaySoundAt's own surface: the positioned counterpart, panned and attenuated
    /// against wherever the listener is.</summary>
    internal static bool PlaySoundAtForGraph(string path, float x, float y, float z,
                                              float volume, float pitch, bool looping, int bus,
                                              float innerCm, float outerCm, out int voice)
    {
        Voice v = Audio.PlayAt(Audio.Load(path), new Vec3(x, y, z), volume, pitch, looping,
                              BusOfPin(bus, "PlaySoundAt"), innerCm, outerCm);
        voice = v.Handle;
        return v.IsValid;
    }

    /// <summary>StopSound: stops one voice by handle. True when there was a handle to stop at all --
    /// NOT whether it was still sounding, which a caller cannot act on anyway by the time it knows.</summary>
    internal static bool StopSoundForGraph(int voice)
    {
        if (voice == 0) return false;
        new Voice(voice).Stop();
        return true;
    }

    /// <summary>IsSoundPlaying: whether that voice is still sounding right now.</summary>
    internal static bool IsSoundPlayingForGraph(int voice) => new Voice(voice).IsPlaying;

    /// <summary>SetListener: where the ears are. A graph driving its own camera needs this, or every
    /// positioned sound pans against the world origin.</summary>
    internal static bool SetListenerForGraph(int entity)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        Audio.SetListener(e.WorldPosition, e.WorldForward, e.WorldRight);
        return true;
    }

    /// <summary>SetBusVolume: how a settings menu gives the player separate SFX and music sliders.</summary>
    internal static bool SetBusVolumeForGraph(int bus, float volume)
    {
        Audio.SetBusVolume(BusOfPin(bus, "SetBusVolume"), volume);
        return true;
    }

    // ================================================================== scene identity, by name
    //
    // The other two members of SetName's own name= family. SetName reflects STRAIGHT into
    // Aver.Scene.Native (it is already scalar-shaped: entity + string -> int), but these two need
    // real wrappers: Entity.Create and Game.Find both return an Entity STRUCT, and hand-emitted IL
    // cannot cheaply unwrap one -- the same reason every other node in this file has a wrapper
    // rather than calling the public API directly (see this class's own header comment).

    /// <summary>Entity.Create(name)'s own surface: makes a new entity and reports its handle.
    /// success is false, and entity 0, only when the scene refused to create one at all (it is out
    /// of entity slots) -- a name that merely duplicates an existing one is fine and expected,
    /// because a name in this engine is not unique (see World::setName's own objectId comment).</summary>
    internal static bool CreateEntityForGraph(string name, out int entity)
    {
        Entity e = Entity.Create(name ?? string.Empty);
        entity = e.Handle;
        return entity != 0;
    }

    /// <summary>Game.Find(name)'s own surface: the FIRST live entity with this name, or 0.
    /// FALSE FOR "NOT FOUND" IS THE POINT -- a miss is an ordinary, expected answer (the thing has
    /// not spawned yet, or was destroyed), not an error, so a graph can branch on it rather than
    /// having to compare the handle against 0 itself. Note the scene's own find is a LINEAR SCAN
    /// over live entities (World::find), so this is not free in a tight loop.</summary>
    internal static bool FindEntityForGraph(string name, out int entity)
    {
        if (string.IsNullOrEmpty(name)) { entity = 0; return false; }
        entity = Game.Find(name).Handle;
        return entity != 0;
    }

    /// <summary>Physics.Ready -- whether aver_phys_init has actually run. A graph that adds bodies
    /// before it has gets silent zeros back from every creator, and had no way to ask until now.</summary>
    internal static bool PhysicsReadyForGraph() => Physics.Ready;

    /// <summary>Physics.FixedStep, seconds. What a graph integrating anything by hand should use
    /// instead of a hardcoded 1/60, so it matches the simulation it is running alongside.</summary>
    internal static float PhysicsFixedStepForGraph() => Physics.FixedStep;

    // ================================================================== Synapse
    //
    // SynapseSteer computes; GetSynapseTarget reads. The two are deliberately independent of each
    // other -- see the design doc's own "Steering" section for why SynapseSteer takes an explicit
    // target rather than reading CSynapseAgent itself: the identical node then does direct chase
    // (target = a seen enemy's live position) as readily as path-following (target =
    // GetSynapseTarget's own output), and neither call needs to know which one a caller is doing.

    /// <summary>GetSynapseTarget's own surface: the entity's CURRENT CSynapseAgent steering target,
    /// as tracked by the native AgentSystem tick (framework_abi.h's aver_fw_synapse_target). FALSE
    /// -- not (0,0,0) -- when the entity carries no CSynapseAgent or its status is not Pathing
    /// (None/Requested/Arrived/Failed all mean "nothing to head toward right now"), the same "absent
    /// is not zero" contract every other relayed graph read in this file already follows.</summary>
    internal static bool SynapseGetTargetForGraph(int entity, out float x, out float y, out float z)
    {
        return Fw.aver_fw_synapse_target(entity, out x, out y, out z) != 0;
    }

    /// <summary>GetSynapsePerception's own surface: the entity's CURRENT CSynapsePerception sight
    /// state, as tracked by the native PerceptionSystem tick (framework_abi.h's
    /// aver_fw_synapse_perception). UNLIKE GetSynapseTarget, the return value here does NOT mean
    /// "can it see something" -- it means "does this entity carry CSynapsePerception at all". A
    /// perceiving agent that currently cannot see its target is a real, common, meaningful state
    /// (canSeeTarget = false), not the same as having no perception component (this method returns
    /// false and every output stays at zero/default only in THAT second case).</summary>
    internal static bool SynapseGetPerceptionForGraph(int entity, out bool canSeeTarget,
                                                       out int lastKnownTarget, out float timeSinceSeen)
    {
        canSeeTarget = false; lastKnownTarget = 0; timeSinceSeen = 0f;
        if (Fw.aver_fw_synapse_perception(entity, out int canSee, out int lastTarget, out float t) == 0)
            return false;
        canSeeTarget = canSee != 0;
        lastKnownTarget = lastTarget;
        timeSinceSeen = t;
        return true;
    }

    /// <summary>SynapseSteer's own surface: "where am I, which way am I facing, where do I want to
    /// go" turned into the forward/right/yawDelta CharacterMove already knows how to consume (see
    /// AverCharacter.Drive's own doc comment for their exact contract: forward/right are -1..1 axis
    /// intent relative to the entity's CURRENT facing, and yawDelta is already a PER-FRAME degree
    /// delta, not a rate). PURE MATH -- no native call beyond the position/forward reads
    /// WorldPositionForGraph/EntityAxisForGraph already use, and no dependency on CSynapseAgent at
    /// all.
    ///
    /// right is always 0: this node steers by TURNING toward the target (yawDelta, clamped to
    /// +-turnRateDegPerSec*dt) rather than by strafing, which is enough for v1 and keeps the two
    /// tuning knobs (how fast do I turn, how close is close enough) independent of a third. forward
    /// eases toward 0 as the misalignment grows (cosine of the yaw error, floored at 0) so an agent
    /// starting up to 180 degrees off its target turns in place first instead of visibly walking
    /// away before it finishes turning -- see the design doc's own "visible jitter, say so" note for
    /// why this is a floor on the roughness, not a promise of none.
    ///
    /// arrived is true, and forward/right/yawDelta are all left at 0, once the entity is within
    /// arriveRadiusCm of the target (measured on the ground plane only, matching the 2.5D grid
    /// Synapse paths across) -- a caller wires that straight into whatever decides "stop calling
    /// CharacterMove now".</summary>
    internal static bool SynapseSteerForGraph(int entity, float dt, float targetX, float targetY, float targetZ,
                                              float turnRateDegPerSec, float arriveRadiusCm,
                                              out float forward, out float right, out float yawDelta,
                                              out bool arrived)
    {
        forward = 0f; right = 0f; yawDelta = 0f; arrived = false;
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;

        Vec3 pos = e.WorldPosition;
        Vec3 fwd = e.WorldForward;

        float dx = targetX - pos.X, dy = targetY - pos.Y;
        float dist = MathF.Sqrt(dx * dx + dy * dy);
        if (dist <= arriveRadiusCm)
        {
            arrived = true;
            return true;
        }

        float desiredYaw = MathF.Atan2(dy, dx) * (180f / MathF.PI);
        float currentYaw = MathF.Atan2(fwd.Y, fwd.X) * (180f / MathF.PI);
        float yawDiff = WrapDegrees(desiredYaw - currentYaw);

        float maxStep = MathF.Abs(turnRateDegPerSec) * MathF.Max(dt, 0f);
        yawDelta = Math.Clamp(yawDiff, -maxStep, maxStep);

        float align = MathF.Cos(yawDiff * (MathF.PI / 180f));
        forward = Math.Clamp(align, 0f, 1f);
        return true;
    }

    /// <summary>Wraps a degree value to (-180, 180] -- the yaw-error convention SynapseSteer needs
    /// so a target 179 degrees one way and 181 the other are recognised as the SAME one-degree turn,
    /// not opposite ends of a 360-degree sweep.</summary>
    private static float WrapDegrees(float degrees)
    {
        degrees %= 360f;
        if (degrees > 180f) degrees -= 360f;
        if (degrees < -180f) degrees += 360f;
        return degrees;
    }

    /// <summary>Entity.Translate: an INCREMENTAL offset, not an assignment. A graph nudging
    /// something every tick wants this rather than reading the position, adding, and writing it
    /// back through three nodes.</summary>
    internal static bool TranslateForGraph(int entity, float x, float y, float z)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        e.Translate(new Vec3(x, y, z));
        return true;
    }

    /// <summary>Entity.SetLocalPosition: where this entity sits RELATIVE TO ITS PARENT.
    ///
    /// The node set had SetParent, SetBodyPosition (which is world space, and physics) and
    /// SetLocalScale -- so a graph could attach a child and resize it, and could not move it. The
    /// visible cost of that gap was the first-person viewmodel: AN_FPCharacter parents the blaster to
    /// the camera node and then has no way to push it forward and down, so it renders centred on the
    /// eye and fills the view. There was nothing wrong with the authoring; the node did not exist.
    ///
    /// LOCAL, not world, and that is the whole point of adding it rather than reusing SetBodyPosition:
    /// a viewmodel has to hold its offset while the camera it hangs from moves and turns every frame,
    /// which is exactly what a parent-relative transform is for. Writing a world position each tick
    /// would fight the parent and lag it by a frame.</summary>
    internal static bool SetLocalPositionForGraph(int entity, float x, float y, float z)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        e.SetLocalPosition(new Vec3(x, y, z));
        return true;
    }

    internal static bool SetLocalScaleForGraph(int entity, float x, float y, float z)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        e.SetLocalScale(new Vec3(x, y, z));
        return true;
    }

    /// <summary>Entity.IsAlive asks the SCENE whether the handle still names anything; IsValid only
    /// asks whether it is non-zero. A graph holding a handle across frames wants the first.</summary>
    internal static bool IsAliveForGraph(int entity) => new Entity(entity).IsAlive;

    /// <summary>True when the entity carries a gameplay CLASS -- an actor -- rather than being a
    /// plain scene node. What a graph branches on before asking for anything actor-shaped.</summary>
    internal static bool IsActorForGraph(int entity) => new Entity(entity).IsActor;

    internal static bool DestroyEntityForGraph(int entity)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        e.Destroy();
        return true;
    }

    // ---- physics -------------------------------------------------------------------------------
    //
    // A BODY IS NOT AN ENTITY, and keeping them apart is the point of this whole block. A body is a
    // Jolt handle with a position, a velocity and a shape; an entity is a scene node that may or may
    // not own one. SetBodyEntity is the bridge, and it matters more than it looks: a body created
    // from a graph reports NO owner to Raycast until something stamps one on, so a trap the graph
    // built is invisible to the graph asking what it hit.
    //
    // Body handles ride on INT pins, never float -- see PrintInt for what a float does to a handle.

    internal static bool BodyPositionForGraph(int body, out float x, out float y, out float z)
    {
        Body b = new Body(body);
        x = y = z = 0.0f;
        if (!b.IsValid) return false;
        Vec3 v = b.Position; x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    internal static bool BodyVelocityForGraph(int body, out float x, out float y, out float z)
    {
        Body b = new Body(body);
        x = y = z = 0.0f;
        if (!b.IsValid) return false;
        Vec3 v = b.Velocity; x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    internal static bool BodyValidForGraph(int body) => new Body(body).IsValid;
    internal static int  BodyCountForGraph() => Physics.BodyCount;

    internal static bool SetBodyPositionForGraph(int body, float x, float y, float z)
    {
        Body b = new Body(body);
        return b.IsValid && b.SetPosition(new Vec3(x, y, z));
    }

    internal static bool SetBodyVelocityForGraph(int body, float x, float y, float z)
    {
        Body b = new Body(body);
        return b.IsValid && b.SetVelocity(new Vec3(x, y, z));
    }

    /// <summary>Body.AddVelocity: an IMPULSE, added to what the body already had, where
    /// SetBodyVelocity replaces it. Knockback, an explosion and a jump pad all want this one.</summary>
    internal static bool AddBodyVelocityForGraph(int body, float x, float y, float z)
    {
        Body b = new Body(body);
        return b.IsValid && b.AddVelocity(new Vec3(x, y, z));
    }

    internal static bool DestroyBodyForGraph(int body)
    {
        Body b = new Body(body);
        return b.IsValid && b.Destroy();
    }

    /// <summary>Stamps the owning entity on a body so Raycast can report it. Without this a body a
    /// graph created answers raycasts with entity 0, which reads as "hit unowned collision".</summary>
    internal static bool SetBodyEntityForGraph(int body, int entity)
    {
        Body b = new Body(body);
        return b.IsValid && b.SetEntity(new Entity(entity));
    }

    internal static bool SetGravityForGraph(float x, float y, float z)
    {
        Physics.SetGravity(new Vec3(x, y, z));
        return true;
    }

    internal static int AddStaticBoxForGraph(float cx, float cy, float cz, float hx, float hy, float hz)
        => Physics.AddStaticBox(new Vec3(cx, cy, cz), new Vec3(hx, hy, hz)).Handle;

    /// <summary>massKg &lt;= 0 derives the mass from the volume, which is what Physics documents and
    /// what a graph leaving the pin unwired gets, since an unwired float pin reads zero.</summary>
    internal static int AddDynamicBoxForGraph(float cx, float cy, float cz, float hx, float hy, float hz, float massKg)
        => Physics.AddDynamicBox(new Vec3(cx, cy, cz), new Vec3(hx, hy, hz), massKg).Handle;

    internal static int AddDynamicSphereForGraph(float cx, float cy, float cz, float radius, float massKg)
        => Physics.AddDynamicSphere(new Vec3(cx, cy, cz), radius, massKg).Handle;

    internal static int AddSensorBoxForGraph(float cx, float cy, float cz, float hx, float hy, float hz)
        => Physics.AddSensorBox(new Vec3(cx, cy, cz), new Vec3(hx, hy, hz)).Handle;

    internal static int AddSensorSphereForGraph(float cx, float cy, float cz, float radius)
        => Physics.AddSensorSphere(new Vec3(cx, cy, cz), radius).Handle;

    internal static bool RaycastAnyForGraph(float ox, float oy, float oz, float dx, float dy, float dz, float maxDist)
        => Physics.RaycastAny(new Vec3(ox, oy, oz), new Vec3(dx, dy, dz), maxDist);

    /// <summary>Physics.SphereCast, shaped like RaycastForGraph above: all results out-parameters,
    /// one call, so the exec compiler can cache them.
    ///
    /// THE ENTITY IS ALWAYS ZERO HERE and that is the engine's own documented limit, not an omission:
    /// the native sweep does not resolve entities. The node therefore reports the BODY, which is
    /// real, and a graph wanting the owner stamps one on with SetBodyEntity and reads it back.</summary>
    internal static void SphereCastForGraph(
        float ox, float oy, float oz, float dx, float dy, float dz, float maxDist, float radius,
        out bool hit, out int body, out float px, out float py, out float pz)
    {
        RaycastHit r = Physics.SphereCast(new Vec3(ox, oy, oz), new Vec3(dx, dy, dz), maxDist, radius);
        hit = r.Hit;
        body = r.Body.Handle;
        px = r.Point.X; py = r.Point.Y; pz = r.Point.Z;
    }

    internal static bool JumpForGraph(int entity)
    {
        Entity e = new Entity(entity);
        AverActor? actor = Actors.Get(e);
        if (actor is not AverCharacter character)
        {
            Log.Warn(actor is null
                ? $"[Graph] Jump: entity {entity} cannot jump -- no live actor is bound to it"
                : $"[Graph] Jump: entity {entity} cannot jump -- its actor is a {actor.GetType().Name}, not an AverCharacter");
            return false;
        }
        return character.Jump();
    }

    // ---- PHYSICS: FORCES, MATERIAL, MOTION, LAYERS, JOINTS -----------------------------------------
    // All of these go through AP.Body/AP.Joint/AP.Physics directly (see the AP alias comment at the
    // top of this file) rather than through Aver.Framework's own Body/Physics shim, which never grew
    // this surface. Same shape as AddBodyVelocityForGraph/SetBodyEntityForGraph above: construct the
    // handle wrapper, check IsValid, call through, return bool (or the value, for a read) -- a joint
    // creator additionally mirrors AddStaticBoxForGraph et al., which do NOT pre-check validity
    // themselves: the native factory already returns handle 0 (Joint.None) for a dead bodyA, and
    // bodyB == 0 is not "dead" here at all, it is the documented sentinel for "join to the world"
    // (see the JOINTS banner in GraphNodeDefs.hpp), so gating on IsValid here would reject the one
    // case the ABI exists to support.

    internal static bool AddForceForGraph(int body, float x, float y, float z)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.AddForce(new AP.Float3(x, y, z));
    }

    internal static bool AddImpulseForGraph(int body, float x, float y, float z)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.AddImpulse(new AP.Float3(x, y, z));
    }

    internal static bool AddTorqueForGraph(int body, float x, float y, float z)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.AddTorque(new AP.Float3(x, y, z));
    }

    internal static bool AddAngularImpulseForGraph(int body, float x, float y, float z)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.AddAngularImpulse(new AP.Float3(x, y, z));
    }

    internal static bool BodyAngularVelocityForGraph(int body, out float x, out float y, out float z)
    {
        AP.Body b = new AP.Body(body);
        x = y = z = 0.0f;
        if (!b.IsValid) return false;
        AP.Float3 v = b.AngularVelocity; x = v.X; y = v.Y; z = v.Z;
        return true;
    }

    internal static bool SetBodyAngularVelocityForGraph(int body, float x, float y, float z)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetAngularVelocity(x, y, z);
    }

    internal static bool SetBodyFrictionForGraph(int body, float friction)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetFriction(friction);
    }

    internal static bool SetBodyRestitutionForGraph(int body, float restitution)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetRestitution(restitution);
    }

    internal static bool SetBodyGravityFactorForGraph(int body, float factor)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetGravityFactor(factor);
    }

    internal static bool SetBodyMassForGraph(int body, float massKg)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetMass(massKg);
    }

    /// <summary>DYNAMIC BODIES ONLY -- a static or kinematic body has infinite mass by definition and
    /// reports 0 here, same as Aver.Physics.Body.Mass itself documents.</summary>
    internal static bool BodyMassForGraph(int body, out float mass)
    {
        AP.Body b = new AP.Body(body);
        mass = 0.0f;
        if (!b.IsValid) return false;
        mass = b.Mass;
        return true;
    }

    internal static bool SetBodyMotionTypeForGraph(int body, int motionType)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetMotionType((AP.MotionType)motionType);
    }

    /// <summary>-1 (not a valid MotionType) for a dead handle -- 0 is Static, a real answer, which is
    /// why `success` exists instead of trusting the int alone.</summary>
    internal static bool BodyMotionTypeForGraph(int body, out int motionType)
    {
        AP.Body b = new AP.Body(body);
        motionType = -1;
        if (!b.IsValid) return false;
        motionType = b.MotionTypeRaw;
        return true;
    }

    internal static bool ActivateBodyForGraph(int body)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.Activate();
    }

    internal static bool BodyActiveForGraph(int body) => new AP.Body(body).IsActive;

    internal static bool SetBodyLayerForGraph(int body, int layer)
    {
        AP.Body b = new AP.Body(body);
        return b.IsValid && b.SetLayer(layer);
    }

    internal static bool BodyLayerForGraph(int body, out int layer)
    {
        AP.Body b = new AP.Body(body);
        layer = -1;
        if (!b.IsValid) return false;
        layer = b.Layer;
        return true;
    }

    /// <summary>No body handle at all -- this edits the world's shared layer-collision matrix, which
    /// Aver.Physics.Physics owns directly rather than any one body.</summary>
    internal static bool SetLayerCollisionForGraph(int layerA, int layerB, bool collide)
        => AP.Physics.SetLayerCollision(layerA, layerB, collide);

    internal static int JointFixedForGraph(int bodyA, int bodyB, float px, float py, float pz,
                                            float axX, float axY, float axZ, float ayX, float ayY, float ayZ)
        => AP.Joint.Fixed(new AP.Body(bodyA), new AP.Body(bodyB), new AP.Float3(px, py, pz),
                           new AP.Float3(axX, axY, axZ), new AP.Float3(ayX, ayY, ayZ)).Handle;

    internal static int JointPointForGraph(int bodyA, int bodyB, float px, float py, float pz)
        => AP.Joint.Point(new AP.Body(bodyA), new AP.Body(bodyB), new AP.Float3(px, py, pz)).Handle;

    internal static int JointDistanceForGraph(int bodyA, int bodyB, float paX, float paY, float paZ,
                                               float pbX, float pbY, float pbZ, float minDist, float maxDist)
        => AP.Joint.Distance(new AP.Body(bodyA), new AP.Body(bodyB), new AP.Float3(paX, paY, paZ),
                              new AP.Float3(pbX, pbY, pbZ), minDist, maxDist).Handle;

    /// <summary>normalAxis MUST be perpendicular to hingeAxis -- the ABI's own requirement (see
    /// physics_joints_abi.h), not something this wrapper can fix up on the graph author's behalf.</summary>
    internal static int JointHingeForGraph(int bodyA, int bodyB, float px, float py, float pz,
                                            float hx, float hy, float hz, float nx, float ny, float nz,
                                            float minAngleRad, float maxAngleRad)
        => AP.Joint.Hinge(new AP.Body(bodyA), new AP.Body(bodyB), new AP.Float3(px, py, pz),
                           new AP.Float3(hx, hy, hz), new AP.Float3(nx, ny, nz), minAngleRad, maxAngleRad).Handle;

    /// <summary>normalAxis MUST be perpendicular to sliderAxis -- same ABI requirement as Hinge's.</summary>
    internal static int JointSliderForGraph(int bodyA, int bodyB, float px, float py, float pz,
                                             float sx, float sy, float sz, float nx, float ny, float nz,
                                             float minCm, float maxCm)
        => AP.Joint.Slider(new AP.Body(bodyA), new AP.Body(bodyB), new AP.Float3(px, py, pz),
                            new AP.Float3(sx, sy, sz), new AP.Float3(nx, ny, nz), minCm, maxCm).Handle;

    /// <summary>state is Aver.Physics.MotorState (0 Off, 1 Velocity, 2 Position). Axis 0 -- every
    /// named joint above has at most one motorised axis; the six-DOF per-axis overload is not
    /// exposed to the graph.</summary>
    internal static bool JointSetMotorForGraph(int joint, int state, float target)
    {
        AP.Joint j = new AP.Joint(joint);
        return j.IsValid && j.SetMotor((AP.MotorState)state, target);
    }

    internal static bool JointSetEnabledForGraph(int joint, bool enabled)
    {
        AP.Joint j = new AP.Joint(joint);
        if (!j.IsValid) return false;
        j.Enabled = enabled;
        return true;
    }

    internal static bool JointRemoveForGraph(int joint)
    {
        AP.Joint j = new AP.Joint(joint);
        return j.IsValid && j.Remove();
    }

    /// <summary>A hinge's angle (radians) or a slider's offset (centimetres). False for a dead handle
    /// AND for a joint type with no single scalar to report -- every type except hinge and slider --
    /// which is why this checks Value()'s own null rather than only IsValid.</summary>
    internal static bool JointValueForGraph(int joint, out float value)
    {
        value = 0.0f;
        AP.Joint j = new AP.Joint(joint);
        if (!j.IsValid) return false;
        float? v = j.Value();
        if (v is null) return false;
        value = v.Value;
        return true;
    }
}
