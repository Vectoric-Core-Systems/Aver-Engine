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
}
