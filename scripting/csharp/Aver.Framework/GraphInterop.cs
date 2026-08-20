// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// A scalar-signature interop surface Aver.Graph's IL compiler calls by reflection.

using Aver.Scene;
using Aver.Scripting;

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
}
