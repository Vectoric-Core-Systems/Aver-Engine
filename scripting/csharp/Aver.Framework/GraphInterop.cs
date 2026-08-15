// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// A scalar-signature interop surface Aver.Graph's IL compiler calls by reflection.

using Aver.Scene;

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
    /// from. <paramref name="entity"/> is the hit Body's raw handle (Body.Handle), not a scene entity
    /// id -- Raycast queries the PHYSICS world, whose bodies are their own handle space (see
    /// Physics.cs's own Body struct); a graph author who needs the SCENE entity that owns a hit body
    /// still needs whatever mapping the project itself keeps, exactly as a C# caller of
    /// Physics.Raycast already would.</summary>
    internal static void RaycastForGraph(
        float originX, float originY, float originZ,
        float dirX, float dirY, float dirZ,
        float maxDistCm,
        out bool hit, out int entity, out float pointX, out float pointY, out float pointZ)
    {
        RaycastHit result = Physics.Raycast(new Vec3(originX, originY, originZ), new Vec3(dirX, dirY, dirZ), maxDistCm);
        hit = result.Hit;
        entity = result.Body.Handle;
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
}
