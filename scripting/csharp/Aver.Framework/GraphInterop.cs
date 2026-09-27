// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// A scalar-signature interop surface Aver.Graph's IL compiler calls by reflection.

using Aver.Scene;
using Aver.Scripting;
// AP: raw Aver.Physics, aliased because this file already has its own Body/Physics/Entity shims
// (Aver.Framework/Physics.cs). Forces, joints, material, motion type and layers have no shim here
// (Physics.cs never grew AddForce/SetFriction/SetMotionType/SetLayer -- predates them, not being
// widened), so wrappers below build an AP.Body/AP.Joint over the same int handle.
using AP = Aver.Physics;

namespace Aver.Framework;

/// <summary>Scalar-signature wrappers over this assembly's public gameplay API, built for
/// System.Reflection.Emit rather than a C# caller: GraphCompiler (Aver.Graph) emits IL that calls
/// these directly by reflection, the same way it already calls Aver.Scene.Native's P/Invoke externs
/// for getfield/setfield, rather than duplicating a second physics/input P/Invoke surface
/// (restrictedSkipVisibility:true on its DynamicMethod is what lets emitted IL reach an internal
/// method here).
///
/// Separate from calling e.g. Physics.Raycast directly because hand-emitted IL cannot cheaply
/// construct a Vec3 or unpack a struct result (RaycastHit's Body/Point/Normal). A float-in /
/// bool,int,float-out signature fits GraphCompiler's existing "call, then read back" emit shape
/// (EmitSetField/EmitExecSideEffect: push inputs, push Ldloca of each output local, Call) -- just
/// wider than GetField/SetField, no new IL pattern needed.</summary>
internal static class GraphInterop
{
    /// <summary>Casts a ray, reporting the first hit as five scalars instead of a RaycastHit struct.
    /// <paramref name="hit"/> is false (rest 0) when nothing is hit within <paramref name="maxDistCm"/>,
    /// mirroring RaycastHit.Hit's own "check this before reading the rest" contract, flattened to scalars.
    /// <paramref name="entity"/> is <see cref="RaycastHit.Entity"/> -- the scene entity stamped on the
    /// hit body/character (Physics.cs's <c>aver_phys_set_entity</c>), NOT the physics Body handle. A
    /// hit against something no entity owns (the landscape heightfield) reports hit=true, entity=0 --
    /// check <paramref name="hit"/> first, since 0 is a real outcome, not an error. Used to return
    /// Body.Handle, which told a graph THAT it hit something but never WHAT (see physics_abi.h's
    /// "Entity association").</summary>
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

    /// <summary>Reads a Vec3-kind scene field as three scalars, the reshaping RaycastForGraph applies
    /// to Physics.Raycast applied to EntityScene.GetVec3/SetVec3's P/Invoke pair (Native.cs's
    /// aver_scene_get_vec/set_vec). <paramref name="fieldId"/> is resolved and kind-checked at COMPILE
    /// time via field=/_fieldResolver (same baked-constant convention as GetField/SetField) -- NOT a
    /// qualified name string, so this needs no SceneIds lookup of its own; x/y/z are
    /// 0 on any rejection (unknown entity, absent component, failed arity check below), mirroring
    /// aver_scene_get_f32's "0f on rejection" contract.
    ///
    /// THE ARITY GUARD IS NOT REDUNDANT WITH THE COMPILE-TIME CHECK: GraphCompiler's FieldResolver is
    /// injectable for unit-testing without a live scene, and a fake resolver claiming kind=Vec3 for a
    /// fieldId the REAL scene disagrees about is what a compile-time-only check can't catch.
    /// aver_scene_get_vec copies `arity` floats into whatever buffer it's given (SceneAbi.cpp) --
    /// against a Quat (arity 4) field with this method's fixed 3-float scratch, that's a buffer
    /// overrun, not a wrong value. EntityScene.GetVec3/SetVec3 check arity==3 for the same
    /// reason.</summary>
    internal static void GetFieldVecForGraph(int entity, int fieldId, out float x, out float y, out float z)
    {
        x = y = z = 0f;
        if (SceneNative.aver_scene_field_arity(fieldId) != 3) return;
        float[] o = Entity.Scratch3;
        if (SceneNative.aver_scene_get_vec(entity, fieldId, o) == 0) return;
        x = o[0]; y = o[1]; z = o[2];
    }

    /// <summary>Writes a Vec3-kind scene field from three scalars -- the write half of
    /// GetFieldVecForGraph (see its comment for the shared fieldId/arity reasoning). Returns
    /// aver_scene_set_vec's real return code (false on unknown entity, wrong arity, read-only field,
    /// or missing component), mirroring EmitExecSideEffect's own SetField-return-code convention.</summary>
    internal static bool SetFieldVecForGraph(int entity, int fieldId, float x, float y, float z)
    {
        if (SceneNative.aver_scene_field_arity(fieldId) != 3) return false;
        float[] s = Entity.Scratch3;
        s[0] = x; s[1] = y; s[2] = z;
        return SceneNative.aver_scene_set_vec(entity, fieldId, s) != 0;
    }

    /// <summary>Spawns a registered class by NAME at a position, unrotated/unscaled -- the scalar
    /// reshaping applied to <see cref="Actors"/>.Spawn(ActorClass, Vec3) (no new native ABI; see
    /// FrameworkAbi.cpp's aver_fw_spawn). Hand-emitted IL can't cheaply construct that overload's
    /// ActorClass/Vec3, so this takes a name and three floats and builds them here.
    ///
    /// RESOLVED AT RUNTIME, NOT BAKED AT COMPILE TIME like field= bakes a fieldId: the scene's field
    /// table is fixed and engine-global before any project loads (FieldResolver's doc comment), but a
    /// project's actor classes are declared by its own Scripts.dll at a boot-order point (LoadScripts
    /// before graph compilation -- confirmed via GameApp.cpp's onInit / SandboxApp.cpp's scripts_.init)
    /// that holds today but isn't asserted anywhere. Baking the handle at compile time would tie the
    /// compiler to that ordering forever and need a second injectable resolver purely for testability --
    /// exactly what the README's "only a compiler case, no new native ABI" framing argues against.
    /// Resolving at invocation time needs neither, since every declared class already exists by the
    /// time a compiled graph's delegate runs.
    ///
    /// Returns entity 0 for an unregistered className (aver_fw_spawn's spawnActor's own "silently did
    /// nothing" convention, FrameworkAbi.cpp: `if (!r) return 0;`), rather than throwing -- a typo in
    /// class= should read as "nothing spawned", the way GetField reads 0f against an unknown
    /// entity.</summary>
    internal static int SpawnForGraph(string className, float x, float y, float z)
    {
        ActorClass c = ActorClass.Find(className);
        if (!c.IsValid) return 0;
        return Actors.Spawn(c, new Vec3(x, y, z)).Handle;
    }

    /// <summary>Reads this frame's mouse delta and wheel as three scalars from ONE call to
    /// aver_fw_input_mouse -- the LOOK half of continuous input (MoveAxisForGraph below is MOVE).
    /// Bypasses Input.MouseDeltaX/MouseDeltaY/MouseWheel: each property calls aver_fw_input_mouse
    /// independently (Input.cs), so wiring IL straight at them would cost three native calls per frame
    /// instead of one. Reuses Entity.Scratch3 to unpack the packed {dx,dy,wheel} buffer, the same
    /// float[]-to-scalars reshape GetFieldVecForGraph applies to aver_scene_get_vec.
    ///
    /// CALLED AT MOST ONCE PER EXEC VISIT ON THE PUSH COMPILER, NOT PER PIN -- a deliberate departure
    /// from GetFieldVec3's uncached "same cost as a memcpy" shape (EmitPullGetFieldVec3). aver_fw_input_mouse
    /// is that same cost class (a three-float struct copy, FrameworkAbi.cpp), so cost alone wouldn't
    /// demand caching -- this was built to an explicit requirement instead: one frame's mouse state must
    /// cost exactly one native call regardless of how many of deltaX/deltaY/wheel a graph reads, not
    /// "cheap enough that repeating it doesn't matter" (see EmitExecMouseDelta / IsExecCapableMouseDeltaType).
    /// Idempotent either way
    /// (aver_fw_input_new_frame mutates state once per engine frame), so calling twice is wasteful,
    /// never unsafe.</summary>
    internal static void MouseDeltaForGraph(out float deltaX, out float deltaY, out float wheel)
    {
        float[] o = Entity.Scratch3;
        Fw.aver_fw_input_mouse(o);
        deltaX = o[0]; deltaY = o[1]; wheel = o[2];
    }

    /// <summary>Reads this frame's WASD/arrow move axis as two scalars -- the MOVE half of continuous
    /// input (see MouseDeltaForGraph for the LOOK half and why both use the exec-cached, push-compiler path).
    /// Unpacks Input.MoveAxis's X (forward)/Y (right); Z isn't exposed since MoveAxis's Z is hardcoded
    /// 0 (Input.cs).
    ///
    /// Input.MoveAxis itself makes ~8 aver_fw_input_key calls (one GetKey per WASD/arrow), unchanged
    /// by wrapping it. This wrapper guarantees those 8 calls happen at most ONCE per exec visit rather
    /// than doubling to ~16 if a graph reads forward AND right independently.</summary>
    internal static void MoveAxisForGraph(out float forward, out float right)
    {
        Vec3 v = Input.MoveAxis;
        forward = v.X; right = v.Y;
    }

    /// <summary>SetMesh's own surface: sets the drawn mesh by asset path, adding a mesh renderer if
    /// the entity has none. NOT a new native ABI, NOT a generalised I64 SetField -- a one-line forward
    /// to <see cref="Entity"/>.SetMesh (EntityScene.cs), which composes EnsureMeshRenderer() +
    /// Assets.ObjectIdOf(path) + SetInt64 -- ObjectIdOf is a pure local FNV1a64 hash (Aver.Scene/Native.cs),
    /// no native call, no I/O, no lookup table, so the only native call this IL reaches is
    /// SetInt64's aver_scene_set_i64. Constructing an <see cref="Entity"/> from a raw handle needs its
    /// `internal` ctor, which this method can call since it lives in the same assembly (unlike
    /// RaycastForGraph/SpawnForGraph, which reshape a different assembly's surface). Returns SetInt64's
    /// real return code (false on unknown entity or a component EnsureMeshRenderer failed to add).</summary>
    internal static bool SetMeshForGraph(int entity, string meshPath) => new Entity(entity).SetMesh(meshPath);

    /// <summary>SetMaterial's own surface: sets the material by name, adding a mesh renderer if the
    /// entity has none. Mirrors SetMeshForGraph above exactly (Entity.SetMaterial composes
    /// EnsureMeshRenderer() + aver_scene_material(0, name) + SetInt, EntityScene.cs).</summary>
    internal static bool SetMaterialForGraph(int entity, string materialName) => new Entity(entity).SetMaterial(materialName);

    /// <summary>AttachToSocket's own surface: hangs <paramref name="entity"/> on a named socket of
    /// <paramref name="parent"/>'s rig. Wraps <see cref="Entity.AttachToSocket"/> (SetParent plus a
    /// component field). IDEMPOTENT, so allowed in the PULL compiler unlike push-only Spawn: attaching
    /// to the same parent/socket twice is the same state, not two attachments.
    ///
    /// TRUE MEANS THE FIELDS WERE WRITTEN, not that the socket resolved -- the parent's rig may not be
    /// loaded yet, and an unmatched name leaves the entity where it is (Entity.AttachToSocket's own
    /// comment).</summary>
    internal static bool AttachToSocketForGraph(int entity, int parent, string socket) =>
        new Entity(entity).AttachToSocket(new Entity(parent), socket);

    /// <summary>GetAnimCurve's own surface: the value of a named curve on the clip
    /// <paramref name="entity"/> is playing, or 0 when there is no such curve.
    ///
    /// A SINGLE FLOAT, NOT A (value, found) PAIR -- a genuine, stated loss: PinType has Float and Bool
    /// and a node could carry both, but the emitter would need two locals/outputs for a distinction a
    /// graph author almost never branches on. TryGetAnimationCurve remains for callers that do
    /// care.</summary>
    internal static float GetAnimCurveForGraph(int entity, string curve) =>
        new Entity(entity).GetAnimationCurve(curve, 0.0f);

    /// <summary>SetSkeleton's own surface: binds a skeleton asset by path, adding a CSkeletalMesh
    /// component if the entity has none. Mirrors SetMeshForGraph (wraps
    /// <see cref="Entity.SetSkeleton"/>, Animation.cs, same AddComponent + ObjectIdOf + SetInt64
    /// shape).</summary>
    internal static bool SetSkeletonForGraph(int entity, string skeletonAsset) =>
        new Entity(entity).SetSkeleton(skeletonAsset);

    /// <summary>PlayAnimation's own surface: plays a clip from the start, adding a CAnimator component
    /// if the entity has none. Wraps <see cref="Entity.PlayAnimation"/> (Animation.cs), which -- unlike
    /// its animation-group siblings -- writes THREE fields (flags, time, clip): loop folds into
    /// CAnimator.flags's AnimatorOnce bit, and the playhead resets to 0 so a re-trigger restarts the
    /// clip. Still idempotent: only one CAnimator per entity, so calling twice with the same args
    /// leaves the same state, not overlapping plays.</summary>
    internal static bool PlayAnimationForGraph(int entity, string clipAsset, bool loop) =>
        new Entity(entity).PlayAnimation(clipAsset, loop);

    /// <summary>SetControlRig's own surface: binds an .ocrig by path so the entity's sampled pose is
    /// modified before skinning, adding a CControlRig component if it has none. Wraps
    /// <see cref="Entity.SetControlRig"/> (Animation.cs). Unlike its two siblings, this component is
    /// registered at RUNTIME: returns false, not a throw, in a host that never registered it -- a graph
    /// authored against a rig is not broken content there, it simply gets no rig.
    ///
    /// Ordering with SetSkeleton is a real constraint the graph author must satisfy (a rig has nothing
    /// to modify until the entity has a skeleton, since that's the pose AnimSystem samples) but this
    /// method does not enforce it: attaching in the other order self-corrects once a skeleton arrives,
    /// since the rig applies per tick from the component, not once at attach.</summary>
    internal static bool SetControlRigForGraph(int entity, string rigAsset, float weight) =>
        new Entity(entity).SetControlRig(rigAsset, weight);

    /// <summary>CharacterMove's own surface: the last Blueprint-parity node, one coarse exec call wrapping
    /// <see cref="AverCharacter"/>.DriveFromGraph -- a forward to the existing `protected`
    /// Drive(dt, moveAxis, yawDeltaDeg, pitchDeltaDeg), which owns the pitch clamp, view mode and
    /// capsule (see DriveFromGraph's own comment for why a seam method, not a reflection hack, is
    /// needed). <paramref name="entity"/> resolves through <see cref="Actors"/>.Get at invocation
    /// time, the same runtime lookup a C# caller would use -- mirrors SpawnForGraph's own "resolved at
    /// invocation, not compile time" reasoning.
    ///
    /// FAILS VISIBLY: false AND a Log.Warn line, with two DIFFERENT messages for two different
    /// mistakes -- no live actor bound at all (dead entity, never spawned, or Actors.Resolver not
    /// installed) vs. a live actor that isn't an <see cref="AverCharacter"/> (wrong actor pointed
    /// here). Returns true with no further reporting on success, mirroring
    /// SetFieldVecForGraph/SetMeshForGraph's "surface the real return code" convention.</summary>
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
    /// WHY THIS EXISTS: <see cref="AverCharacter"/> keeps _yaw/_pitch private, publishing the aim only
    /// as a Quat (<see cref="AverCharacter.LookDirection"/>) the graph vocabulary can't read (GetField
    /// is F32-only, GetFieldVec3 needs FieldKindVec3, see RequireVec3Field). Rebuilding it graph-side
    /// from mouse deltas would duplicate Character.cs's own state (Drive's pitch CLAMP, PitchMin/PitchMax)
    /// and drift.
    ///
    /// BOTH HALVES, ONE CALL: a direction alone can't build a ray, and the eye must sit ABOVE the
    /// capsule or the ray hits the character firing it (test-content's AN_Playable hand-computed a
    /// constant z=290 for that reason; <see cref="AverCharacter.EyePosition"/> is the correct number).
    ///
    /// Fails the same visible way CharacterMoveForGraph does, leaving every out-parameter at zero -- a
    /// zero direction makes Raycast a no-op rather than firing somewhere arbitrary.</summary>
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
    /// A first-person weapon must be parented to the view node, not the pawn: parenting to the
    /// character keeps it fixed while the camera pitches around it; parenting to the view moves it
    /// with the eye, which is what a viewmodel is. <see cref="AverCharacter"/> creates that node in
    /// EnsureView() and publishes it only as <see cref="AverCharacter.View"/> -- a graph had SetParent
    /// and SetMesh but no way to NAME the thing to parent to, the same gap LookDirection had before
    /// GetForward.
    ///
    /// Returns 0/false when there is no character, or its view node doesn't exist yet (EnsureView is
    /// lazy, so OnStart before CharacterMove legitimately gets nothing) -- a "try again next tick",
    /// not an error, so this fails quietly rather than warning every frame.</summary>
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

    /// <summary>Jump's own surface: one call into <see cref="AverCharacter.Jump"/>, which refuses
    /// mid-air by returning false. THE GROUNDED CHECK IS NOT THIS NODE'S TO MAKE: Jump() already asks
    /// the physics character whether it's standing on something (Phys.aver_phys_character_grounded)
    /// and declines if not -- re-testing here would risk disagreeing with that answer, so a graph
    /// wired straight to a key gets single jumps, no flight, for free.
    ///
    /// THE RETURN IS THE INTERESTING PART, and why this reports `jumped` rather than nothing: a jump
    /// SOUND, animation or counter needs to know whether the jump actually happened, not just that the
    /// key was pressed. False means airborne, ordinary and frequent, so unlike the wrong-actor case
    /// below it is not logged -- a warning every frame the key is held would be noise, not
    /// diagnosis.</summary>
    /// <summary>Print's surface: one log line, labelled with the graph node's own id.
    ///
    /// BEFORE THIS, a graph could only observe itself by routing a value all the way to an OUT record
    /// (which has to reach the graph's own output to exist at all) and reading it back off GraphHost's
    /// log -- one value per graph, silent about which branch ran or whether a chain ran at all. FLOAT,
    /// NOT A STRING, because PinType has no string (Float, Int, Bool, Exec only).</summary>
    internal static void PrintForGraph(string label, float value)
    {
        Log.Info($"[Graph] {label} = {value}");
    }

    /// <summary>Print for an INT pin -- a correctness fix, not a convenience. A float32 has 24
    /// mantissa bits, so above 16777216 it holds only EVEN integers, and this engine's ENTITY HANDLES
    /// START AT 16777216. Routing a handle through IntToFloat to reach Print silently rounds it, which
    /// reads exactly like the engine returning the wrong entity -- cost an hour chasing a bug that
    /// wasn't there.</summary>
    internal static void PrintIntForGraph(string label, int value)
    {
        Log.Info($"[Graph] {label} = {value}");
    }

    /// <summary>PrintString: an authored message, node id kept as a prefix so two nodes carrying the
    /// same text stay tellable apart. The one print needing nothing wired but exec -- the node for
    /// "did control flow reach here".
    ///
    /// The "[Graph] " prefix is load-bearing: the editor's on-screen print overlay filters the engine
    /// log on exactly that prefix (SandboxApp::logSink), so a line without it lands in the log and
    /// never appears in the viewport.</summary>
    internal static void PrintStringForGraph(string label, string text)
    {
        Log.Info($"[Graph] {label}: {text}");
    }

    // ---- node-hit recording, for the editor's execution highlighting -----------------------------
    // Every exec node calls this as it runs, so the editor can show which nodes are executing.
    //
    // OFF UNLESS THE EDITOR ASKS: hot path of every exec node of every instance, so off it must cost a
    // static bool test and nothing else (no alloc, no dict probe, no time read) -- a packaged game
    // never turns it on. NOT THE LOG CHANNEL: PrintStringForGraph's interpolation + Log.Info under the
    // core log mutex + a cross-thread filter would make the profiler part of what it profiles if paid
    // per node per frame.
    //
    // KEYED BY (GRAPH NAME, NODE ID), both compile-time constants, not entity (the compiled method's
    // args come from the graph's PARAM list, no "entity is always arg 0" to lean on, and a graph with
    // no entity PARAM has none to report). Keying by graph name is also what the editor wants: its
    // canvas shows a CLASS, and any instance running a node should light it.
    //
    // LAST-HIT TIME, NEVER A COUNTER: a diamond in the exec graph (two branch arms rejoining) emits the
    // shared node's IL twice at compile time, so a counter would over-report -- "when did this last
    // run" is the honest measure a fading highlight needs.
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

    // ---- tags and visibility ---------------------------------------------------------------------
    // A TAG IS A BITMASK, NOT A STRING: Entity.Tags is a uint over CTags.bits, so an ordinary int pin
    // carries it directly -- combine masks with a bitwise Or node, test several at once, stash one in
    // a VAR -- unlike class=/name=/field=/var=, which needed a compile-time NODE attribute since
    // PinType has no string.
    //
    // int RATHER THAN uint: PinType has no unsigned type, and inventing one just to carry a bit
    // pattern would be a new pin type for one reinterpretation, so `unchecked((uint))` is the whole
    // conversion (same as Entity.Tags does in reverse, EntityScene.cs). A mask with the top bit set
    // arrives as a negative int and works.

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

    /// <summary>Game.SaveGame / LoadGame, unchanged -- see their own doc comments for the atomic write
    /// and the "destroy everything, then restore" load. Direct forwards, needing no Entity ctor
    /// (unlike SetMesh/SetMaterial), same as IsPlayingForGraph above.
    ///
    /// LOADGAMEFORGRAPH CAN DESTROY THE ENTITY CALLING IT: if a "loadgame" node fires from inside this
    /// entity's own graph, the world tear-down runs OnEndPlay on it mid-exec like any other. Not a new
    /// hazard -- Game.LoadGame already carries it, and Blueprint's own Load Game node has the identical
    /// footgun.</summary>
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

    /// <summary>One of the entity's WORLD axes: 0 forward, 1 right, 2 up. Three nodes share this one
    /// surface since they differ only in which axis they ask for, cheaper than three near-identical
    /// methods. Distinct from GetForward, which reads an AverCharacter's look direction including its
    /// pitch clamp -- this is the transform's own orientation and works on anything.</summary>
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
    // sound= is a PATH, a NODE-line attribute rather than a pin, same reason as SetName: chosen at
    // edit time and PinType has no String. Load is cached native-side (same path -> same handle without
    // redecoding), so calling these every time the node runs costs a dictionary probe, not a decode.

    /// <summary>Turns a graph's raw int pin into a <see cref="Bus"/>, saying so when it is not one.</summary>
    ///
    /// <remarks>THE ONE PLACE AN OUT-OF-RANGE BUS CAN ARRIVE: every other caller names a bus through
    /// the enum, but a graph pin is an integer someone typed into a node. Native <c>busOf</c> folds
    /// anything unrecognised into Sfx, silently -- a Set Bus Volume node set to 7 moves the SFX slider
    /// with nothing saying why. This doesn't change that behaviour, it just stops it being
    /// silent.</remarks>
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
    // The other two members of SetName's name= family. SetName reflects straight into
    // Aver.Scene.Native (already scalar-shaped: entity + string -> int), but Entity.Create/Game.Find
    // both return an Entity STRUCT, which hand-emitted IL can't cheaply unwrap -- the same reason every
    // wrapper in this file exists at all (see the class header comment).

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

    /// <summary>Game.Find(name)'s own surface: the FIRST live entity with this name, or 0. FALSE FOR
    /// "NOT FOUND" IS THE POINT -- a miss (not spawned yet, or destroyed) is expected, not an error, so
    /// a graph can branch on it directly. Note World::find is a LINEAR SCAN over live entities, so
    /// this isn't free in a tight loop.</summary>
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
    // SynapseSteer computes; GetSynapseTarget reads. Deliberately independent (design doc's
    // "Steering" section): SynapseSteer takes an explicit target rather than reading CSynapseAgent
    // itself, so the identical node does direct chase (target = a seen enemy's position) as readily as
    // path-following (target = GetSynapseTarget's output), with neither call knowing which.

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
    /// aver_fw_synapse_perception). UNLIKE GetSynapseTarget, the return
    /// value here does NOT mean "can it see something" -- it means "does this entity carry
    /// CSynapsePerception at all". canSeeTarget=false while perceiving is a real, common state,
    /// distinct from having no perception component (only THAT case returns false with zeroed
    /// outputs).</summary>
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

    /// <summary>SynapseSteer's own surface: "where am I, which way am I facing, where do I want to go"
    /// turned into the forward/right/yawDelta CharacterMove already consumes (AverCharacter.Drive's
    /// contract: forward/right are -1..1 relative to CURRENT facing, yawDelta a PER-FRAME degree
    /// delta, not a rate). PURE MATH -- no native call beyond the position/forward reads
    /// WorldPositionForGraph/EntityAxisForGraph use, and no CSynapseAgent dependency.
    ///
    /// right is always 0: turns TOWARD the target (yawDelta clamped to +-turnRateDegPerSec*dt) rather
    /// than strafing, keeping the two tuning knobs (turn rate, arrive radius) independent of a third.
    /// forward eases toward 0 as misalignment grows (cosine of yaw error, floored at 0) so an agent up
    /// to 180 degrees off turns in place first instead of visibly walking away (design doc's "visible
    /// jitter, say so" note: a floor on roughness, not a promise of none).
    ///
    /// arrived is true, with forward/right/yawDelta left at 0, once within arriveRadiusCm of the
    /// target (ground plane only, matching the 2.5D grid Synapse paths across).</summary>
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

    /// <summary>Entity.SetLocalPosition: where this entity sits RELATIVE TO ITS PARENT. The node set
    /// had SetParent, SetBodyPosition (world space, physics) and SetLocalScale -- a graph could attach
    /// and resize a child but not move it (the visible cost: AN_FPCharacter's blaster, parented to the
    /// camera with no way to push it forward and down, rendered centred on the eye). Not an authoring
    /// bug -- the node simply did not exist.
    ///
    /// LOCAL, not world, is the point over reusing SetBodyPosition: a viewmodel must hold its offset
    /// while the camera it hangs from moves AND TURNS every frame; a world position each tick would
    /// fight the parent and lag it by a frame.</summary>
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

    /// <summary>Entity.SetLocalRotation, in DEGREES, as three float pins. The palette could move,
    /// scale, parent and destroy an entity and could not TURN one, while Entity.SetLocalRotation sat
    /// fully implemented and unreachable.
    ///
    /// DEGREES THROUGH A Rot, NOT A RAW QUATERNION, forced rather than chosen: PinType is Float, Int,
    /// Bool, Exec -- no Vec4/quaternion, no string either. Rot's ToQuat uses the same intrinsic Z-Y-X
    /// composition .ocmap's PLACE records use, so a value typed into a graph means what it means in a
    /// level file -- reusing that conversion avoids a second Euler-to-quaternion that would disagree
    /// with every placed actor.</summary>
    internal static bool SetLocalRotationForGraph(int entity, float yaw, float pitch, float roll)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        e.SetLocalRotation(new Rot(yaw, pitch, roll).ToQuat());
        return true;
    }

    /// <summary>Turns an entity to face a world point, the single most reached-for rotation in a game.
    /// YAW AND PITCH ONLY, ROLL ZEROED -- what "look at" means for a turret, character or camera. A
    /// caller wanting roll uses SetLocalRotation instead.
    ///
    /// DERIVED FROM THE DIRECTION in the engine's axis convention (Forward +X, Right +Y, Up +Z): yaw
    /// is atan2(dy, dx), pitch the rise over horizontal run, NEGATED because Rot's pitch turns about
    /// +Y (Right) and a right-handed turn about Right tips the nose DOWN -- the wrong sign reads as a
    /// targeting bug, not a sign error. A TARGET DIRECTLY ABOVE/BELOW leaves no horizontal run, so yaw
    /// is meaningless (atan2(0,0)=0 would snap to +X): straight up/down is answered, keeping the
    /// existing yaw.
    ///
    /// LOCAL ROTATION, WORLD TARGET: correct for an unparented actor (the common case). A PARENTED one
    /// turns relative to its parent, so a child of a rotating mount won't point where this asks --
    /// the node's name promises world behaviour but the transform it writes is local, the same
    /// asymmetry SetLocalPosition's comment calls out.</summary>
    internal static bool LookAtForGraph(int entity, float targetX, float targetY, float targetZ)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;

        Vec3 from = e.LocalPosition;
        float dx = targetX - from.X, dy = targetY - from.Y, dz = targetZ - from.Z;

        float horiz = System.MathF.Sqrt(dx * dx + dy * dy);
        const float r2d = 180f / System.MathF.PI;

        // Degenerate only in the horizontal plane: a target directly above or below still has a
        // perfectly good pitch, so answer that and leave yaw where it was rather than snapping.
        if (horiz < 1e-4f)
        {
            if (System.MathF.Abs(dz) < 1e-4f) return true;   // the target IS the entity: nothing to face
            Rot keep = new Rot(0f, dz > 0f ? -90f : 90f, 0f);
            e.SetLocalRotation(keep.ToQuat());
            return true;
        }

        float yaw   = System.MathF.Atan2(dy, dx) * r2d;
        float pitch = -System.MathF.Atan2(dz, horiz) * r2d;   // see the sign note above
        e.SetLocalRotation(new Rot(yaw, pitch, 0f).ToQuat());
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
    // A BODY IS NOT AN ENTITY, the point of this whole block: a body is a Jolt handle with position,
    // velocity and shape; an entity is a scene node that may or may not own one. SetBodyEntity bridges
    // them -- a body created from a graph reports NO owner to Raycast until stamped, so a trap the
    // graph built is invisible to a graph asking what it hit.
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

    /// <summary>Physics.SphereCast, shaped like RaycastForGraph: all results out-parameters, one call,
    /// cacheable by the exec compiler.
    ///
    /// THE ENTITY IS ALWAYS ZERO HERE -- the engine's own documented limit, not an omission: the
    /// native sweep does not resolve entities. The node reports the BODY instead; a graph wanting the
    /// owner stamps one on with SetBodyEntity and reads it back.</summary>
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
    // Goes through AP.Body/AP.Joint/AP.Physics directly (see the AP alias comment at the file top),
    // since Aver.Framework's own Body/Physics shim never grew this surface. Same shape as
    // AddBodyVelocityForGraph/SetBodyEntityForGraph: construct the wrapper, check IsValid, call
    // through, return bool or value.
    //
    // Joint creators do NOT pre-check validity like AddStaticBoxForGraph et al. do: the native factory
    // returns handle 0 (Joint.None) for a dead bodyA, and bodyB == 0 is not "dead" here at all -- it's
    // the documented sentinel for "join to the world" (JOINTS banner, GraphNodeDefs.hpp) -- gating on
    // IsValid here would reject the one case the ABI exists to support.

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

    // ================================================================== named input actions (rebindable)
    // ActionHandleForGraph is how an InputAction/InputActionPressed/InputActionReleased node's own
    // action= attribute (GraphCompiler's EmitInputAction family, contract D) resolves to the native
    // handle aver_fw_action_held/pressed/released/value2 already read -- a graph carrying that
    // attribute never constructs an InputAction wrapper at all, so this is a SECOND, independent
    // name->handle cache, not a reuse of InputAction's own s_byHandle (EnhancedInput.cs), which is
    // keyed by handle and only knows actions THIS runtime declared.

    private static readonly Dictionary<string, int> s_actionHandleByName = new();

    /// <summary>The native handle for a named action, resolving via aver_fw_action_find and caching by
    /// name. 0 for an unregistered name -- NEVER CACHED, so a graph asking before a scheme or script
    /// registers the action gets the right answer once it does, rather than being stuck at 0.</summary>
    internal static int ActionHandleForGraph(string actionName)
    {
        if (string.IsNullOrEmpty(actionName)) return 0;
        if (s_actionHandleByName.TryGetValue(actionName, out int handle)) return handle;
        handle = Fw.aver_fw_action_find(actionName);
        if (handle != 0) s_actionHandleByName[actionName] = handle;
        return handle;
    }

    /// <summary>SaveInputBindings' own surface: EnhancedInput.SaveAllBindings(). False when no context
    /// is pushed at all, otherwise whether the settings store reached disk.</summary>
    internal static bool SaveInputBindingsForGraph() =>
        EnhancedInput.ContextCount != 0 && EnhancedInput.SaveAllBindings();

    /// <summary>LoadInputBindings' own surface: EnhancedInput.LoadAllBindings(). LoadBindings has no
    /// failure mode beyond a missing/stale key, tolerated silently (InputMappingContext.LoadBindings's
    /// own comment) -- so the only false case here is nothing pushed to load onto.</summary>
    internal static bool LoadInputBindingsForGraph()
    {
        if (EnhancedInput.ContextCount == 0) return false;
        EnhancedInput.LoadAllBindings();
        return true;
    }

    /// <summary>ResetInputBindings' own surface: EnhancedInput.ResetAllBindings(). ResetToDefaults
    /// can't itself fail, so this mirrors Save/LoadInputBindingsForGraph's "false means nothing to act
    /// on" convention rather than inventing a native failure mode that doesn't exist.</summary>
    internal static bool ResetInputBindingsForGraph()
    {
        if (EnhancedInput.ContextCount == 0) return false;
        EnhancedInput.ResetAllBindings();
        return true;
    }

    /// <summary>RebindAction's own surface: EnhancedInput.RebindAction(actionName, slot, key) -- see
    /// that method's own comment for the per-context slot walk and the source/defined-value checks
    /// that make it return false.</summary>
    internal static bool RebindActionForGraph(string actionName, int slot, int key) =>
        EnhancedInput.RebindAction(actionName, slot, key);

    /// <summary>GetActionKey's own surface: the raw key/button/axis currently bound at
    /// <paramref name="actionName"/>'s <paramref name="slot"/>-th binding, or -1 when there is no such
    /// binding at all -- the node's own `bound` pin is a graph-side comparison against -1, the same
    /// "sentinel the node compares itself" shape JointValueForGraph's own -1 already uses above.</summary>
    internal static int GetActionKeyForGraph(string actionName, int slot) =>
        EnhancedInput.TryGetBindingKey(actionName, slot, out int rawKey, out _) ? rawKey : -1;

    /// <summary>GetPressedKey's own surface: Input.FirstKeyPressedThisFrame() -- what a "press any key
    /// to rebind" menu polls every frame while it waits for the player to press something.</summary>
    internal static int GetPressedKeyForGraph() => Input.FirstKeyPressedThisFrame();
}
