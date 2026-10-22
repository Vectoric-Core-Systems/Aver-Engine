// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the Physical Surface Aver Node nodes call (scripting/csharp/Aver.Graph/GraphPhysicalSurfaceNodeTable.cs;
// palette mirror sandbox/src/GraphNodeDefsPhysicalSurface.hpp). Each is a thin wrapper over PhysicalSurfaces, the surface and
// body-scale members of Entity (EntitySurface.cs) and the registry in Aver.Physics. A node never throws: a dead entity, an entity
// with no rigid body, an unknown surface or a missing native library answers false/0, logged once per node type (FamiliesBLog).
// Entities are ints; a surface is the int id AN_SurfaceId and AN_LandscapeSurface already produce.

using Aver.Scene;
using AP = Aver.Physics;

namespace Aver.Framework;

internal static class SurfaceGraph
{
    private const string NoBody = "the entity is dead or has no built rigid body";

    /// <summary>AN_RaycastSurface: a ray that names what it hit. True on a hit; every output is 0 on a miss.</summary>
    internal static bool RaycastSurfaceForGraph(float originX, float originY, float originZ, float dirX, float dirY, float dirZ,
                                                float maxDist, int layerMask, out int entity,
                                                out float pointX, out float pointY, out float pointZ,
                                                out float normalX, out float normalY, out float normalZ,
                                                out float distance, out int surfaceId, out float friction, out float restitution,
                                                out int tag)
    {
        entity = 0; pointX = pointY = pointZ = 0f; normalX = normalY = normalZ = 0f; distance = 0f;
        surfaceId = 0; friction = restitution = 0f; tag = 0;
        try
        {
            SurfaceHit h = PhysicalSurfaces.Raycast(new Vec3(originX, originY, originZ), new Vec3(dirX, dirY, dirZ), maxDist, unchecked((uint)layerMask));
            if (!h.Hit) return false;
            entity = h.Entity.Handle;
            pointX = h.Point.X; pointY = h.Point.Y; pointZ = h.Point.Z;
            normalX = h.Normal.X; normalY = h.Normal.Y; normalZ = h.Normal.Z;
            distance = h.DistanceCm; surfaceId = h.SurfaceId; friction = h.Friction; restitution = h.Restitution; tag = h.Tag;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_RaycastSurface", e); }
    }

    /// <summary>AN_SurfaceBeneath: the surface under an entity, probing <paramref name="probeCm"/> down.</summary>
    internal static bool BeneathForGraph(int entity, float probeCm, out int surfaceId, out float friction, out float restitution, out int tag)
    {
        surfaceId = 0; friction = restitution = 0f; tag = 0;
        try
        {
            SurfaceHit h = PhysicalSurfaces.Beneath(new Entity(entity), probeCm);
            if (!h.Hit) return false;
            surfaceId = h.SurfaceId; friction = h.Friction; restitution = h.Restitution; tag = h.Tag;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SurfaceBeneath", e); }
    }

    /// <summary>AN_SurfaceInfo: the registered numbers of a surface id; found is false for an id nothing has used.</summary>
    internal static bool InfoForGraph(int surfaceId, out float friction, out float restitution, out int tag)
    {
        friction = restitution = 0f; tag = 0;
        try
        {
            AP.SurfaceInfo? info = PhysicalSurfaces.Info(surfaceId);
            if (info is null) return false;
            friction = info.Value.Friction; restitution = info.Value.Restitution; tag = info.Value.Tag;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SurfaceInfo", e); }
    }

    /// <summary>AN_SetBodySurface: labels the entity's rigid body with a surface; copyPhysics also copies the surface's
    /// friction and restitution into the body once.</summary>
    internal static bool SetBodySurfaceForGraph(int entity, int surfaceId, bool copyPhysics)
    {
        try { return FamiliesBLog.Ok("AN_SetBodySurface", new Entity(entity).SetPhysicalSurface(surfaceId, copyPhysics), NoBody); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetBodySurface", e); }
    }

    /// <summary>AN_RegisterSurface: adds or updates the surface named by the node's name= attribute.</summary>
    internal static bool RegisterForGraph(string name, float friction, float restitution, int tag, out int surfaceId)
    {
        surfaceId = 0;
        try
        {
            if (string.IsNullOrEmpty(name)) return FamiliesBLog.Ok("AN_RegisterSurface", false, "the node has no name= attribute");
            surfaceId = PhysicalSurfaces.Register(name, friction, restitution, tag);
            return FamiliesBLog.Ok("AN_RegisterSurface", surfaceId != 0, "the physics module refused the surface (an empty name, a non-finite number or a hash collision)");
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_RegisterSurface", e); }
    }

    /// <summary>AN_TuneSurface: changes the numbers of a surface id; live for every body on it.</summary>
    internal static bool TuneForGraph(int surfaceId, float friction, float restitution, int tag)
    {
        try { return FamiliesBLog.Ok("AN_TuneSurface", PhysicalSurfaces.Tune(surfaceId, friction, restitution, tag), "no surface with that id is known"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_TuneSurface", e); }
    }

    /// <summary>AN_SetBodyScale: the entity's rigid body collision shape at an absolute scale (physics only; pair it with the
    /// transform scale to scale what is drawn).</summary>
    internal static bool SetBodyScaleForGraph(int entity, float scaleX, float scaleY, float scaleZ, bool keepMass)
    {
        try { return FamiliesBLog.Ok("AN_SetBodyScale", new Entity(entity).SetBodyScale(new Vec3(scaleX, scaleY, scaleZ), keepMass), NoBody + ", or the scale has a zero component"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetBodyScale", e); }
    }
}
