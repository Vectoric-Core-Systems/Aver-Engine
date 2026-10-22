// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Script-facing physical surfaces: what a ray or an entity's feet touch (its surface name, id, friction, restitution and tag),
// and the registry that holds the numbers. A surface is a name hashed to a 32-bit id (Surface.IdOf), the same id a physics
// raycast, a contact and Landscape.Sample report, so a footstep script compares integers whatever it hit. The registry and
// the detail queries are Aver.Physics.Surfaces / Aver.Physics.Physics.RaycastDetail; this file converts to the engine-wide
// Vec3 / Entity types scripts already use and adds the landscape fallback.

using Aver.Scene;
using AP = Aver.Physics;

namespace Aver.Framework;

/// <summary>What a ray or a query under an entity touched. <see cref="Hit"/> is false on a miss and every other field is then
/// its default. <see cref="Friction"/> and <see cref="Restitution"/> are the numbers the surface carries (-1 when it defers to
/// the body it is on), not necessarily what a contact applied. <see cref="Entity"/> is the scene entity that owns the body hit
/// (<see cref="Framework.Entity.None"/> for terrain and other unowned geometry).</summary>
public readonly record struct SurfaceHit(bool Hit, Entity Entity, Vec3 Point, Vec3 Normal, float DistanceCm, int SurfaceId,
                                         string SurfaceName, float Friction, float Restitution, int Tag);

/// <summary>Physical surfaces for scripts. Main-thread, between physics steps; nothing here throws (a missing physics library
/// answers a miss, 0 or false).</summary>
public static class PhysicalSurfaces
{
    private const float ProbeLiftCm = 10f;   // Beneath starts a little above the pivot so a resting body does not start inside the ground

    /// <summary>Casts a ray and reports what it hit: the surface at the exact triangle, cell or compound leaf, with its numbers.
    /// A hit on terrain whose heightfield carries no per-cell surface answers from <see cref="Landscape.Sample(float, float)"/>,
    /// so the ground still names its dominant paint layer.</summary>
    public static SurfaceHit Raycast(Vec3 origin, Vec3 direction, float maxDistanceCm, uint layerMask = uint.MaxValue)
    {
        try
        {
            return From(AP.Physics.RaycastDetail(Physics.ToFloat3(origin), Physics.ToFloat3(direction), maxDistanceCm, layerMask));
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return default; }
    }

    /// <summary>What an entity stands on: a ray straight down from just above its position, <paramref name="probeCm"/> long,
    /// ignoring the entity's own rigid body. A miss (the entity is dead, in the air or over nothing) is <c>Hit == false</c>.</summary>
    public static SurfaceHit Beneath(Entity entity, float probeCm = 150f)
    {
        try
        {
            if (!entity.IsAlive) return default;
            Vec3 p = entity.WorldPosition;
            var origin = new AP.Float3(p.X, p.Y, p.Z + ProbeLiftCm);
            int ignore = entity.RigidBodyHandle;
            return From(AP.Physics.RaycastDetail(origin, new AP.Float3(0f, 0f, -1f), probeCm + ProbeLiftCm, uint.MaxValue,
                                                 new AP.Body(ignore)));
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return default; }
    }

    /// <summary>The id of a REGISTERED surface called <paramref name="name"/> (a Surfaces.ocsurf entry, a landscape layer or
    /// <see cref="Register"/>); 0 when no surface of that name is registered. <see cref="Surface.IdOf"/> is the pure hash.</summary>
    public static int Find(string name)
    {
        int id = Surface.IdOf(name);
        try { return id != 0 && AP.Surfaces.TryGet(id, out AP.SurfaceInfo info) && info.Name == name ? id : 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    /// <summary>Adds or updates a named surface; returns its id (0 when refused). Friction and restitution below 0 defer to the
    /// body; both clamp to 10.</summary>
    public static int Register(string name, float friction = -1f, float restitution = -1f, int tag = 0)
    {
        try { return AP.Surfaces.Register(name, friction, restitution, tag); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    /// <summary>Changes the numbers of a surface; live for every body on it. False for an id nothing has used.</summary>
    public static bool Tune(int surface, float friction, float restitution, int tag)
    {
        try { return AP.Surfaces.Tune(surface, friction, restitution, tag); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>The registry entry of <paramref name="surface"/> (name, friction, restitution, tag), or null when the id is unknown.</summary>
    public static AP.SurfaceInfo? Info(int surface)
    {
        try { return AP.Surfaces.TryGet(surface, out AP.SurfaceInfo info) ? info : null; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return null; }
    }

    private static SurfaceHit From(AP.SurfaceHit h)
    {
        if (!h.Hit) return default;
        int id = h.SurfaceId, tag = h.Tag;
        float friction = h.Friction, restitution = h.Restitution;
        string name = id != 0 ? Surface.NameOf(id) : string.Empty;
        if (id == 0 && h.Entity == 0)
        {
            // Unowned ground whose shape carries no surface: the landscape knows what is painted there.
            GroundSample g = Landscape.Sample(h.Point.X, h.Point.Y);
            if (g.Found && g.SurfaceId != 0)
            {
                id = g.SurfaceId; name = g.Surface; friction = g.Friction; restitution = g.Restitution;
                if (AP.Surfaces.TryGet(id, out AP.SurfaceInfo info)) tag = info.Tag;
            }
        }
        return new SurfaceHit(true, new Entity(h.Entity), Physics.ToVec3(h.Point), Physics.ToVec3(h.Normal), h.DistanceCm, id, name,
                              friction, restitution, tag);
    }
}
