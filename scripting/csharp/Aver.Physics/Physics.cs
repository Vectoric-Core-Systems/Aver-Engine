// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The simulation as a whole: starting/stopping it, gravity, stepping, body/sensor/soft-body creation,
// the global water plane, the collision-layer matrix, queries, and the contact/overlap event queues.
// Per-body state lives on <see cref="Body"/>; joints on <see cref="Joint"/>; the extra rigid-body
// shapes on <see cref="Shapes"/>; the walking capsule on <see cref="CharacterBody"/>.

using System;
using System.Collections.Generic;

namespace Aver.Physics;

/// <summary>Two solid bodies that began touching during the last step.</summary>
public readonly struct ContactEvent
{
    public Body A { get; }
    public Body B { get; }
    /// <summary>Where they touched, centimetres.</summary>
    public Float3 Point { get; }
    /// <summary>The contact normal, unit length.</summary>
    public Float3 Normal { get; }

    internal ContactEvent(int a, int b, Float3 point, Float3 normal) { A = new Body(a); B = new Body(b); Point = point; Normal = normal; }

    /// <summary>The other body in this contact; <see cref="Body.None"/> if <paramref name="one"/> is neither.</summary>
    public Body Other(Body one) => one.Handle == A.Handle ? B : one.Handle == B.Handle ? A : Body.None;
}

/// <summary>Something entering or leaving a sensor volume during the last step.</summary>
public readonly struct OverlapEvent
{
    public Body Sensor { get; }
    public Body Other { get; }
    /// <summary>True on entering, false on leaving.</summary>
    public bool Entered { get; }

    internal OverlapEvent(int sensor, int other, bool entered) { Sensor = new Body(sensor); Other = new Body(other); Entered = entered; }
}

/// <summary>What a raycast or sphere cast found.</summary>
public readonly struct RaycastHit
{
    /// <summary><see cref="Body.None"/> when nothing was hit.</summary>
    public Body Body { get; }
    /// <summary>The scene entity id stamped on the hit handle via <see cref="Body.SetEntity"/>, or 0
    /// when nothing ever did -- a REAL hit against something no entity owns (a landscape heightfield),
    /// not a miss. Check <see cref="Hit"/> first, never this alone, to tell the two apart. Always 0 from
    /// <see cref="Physics.SphereCast"/>/<see cref="Physics.SphereCastEx"/>, which do not resolve entities.</summary>
    public int Entity { get; }
    /// <summary>Where the ray met the surface, centimetres.</summary>
    public Float3 Point { get; }
    /// <summary>The surface normal at that point, unit length.</summary>
    public Float3 Normal { get; }
    /// <summary>True when something was hit. Check this before reading the rest.</summary>
    public bool Hit => Body.IsValid;

    internal RaycastHit(int body, int entity, Float3 point, Float3 normal) { Body = new Body(body); Entity = entity; Point = point; Normal = normal; }
}

/// <summary>The simulation itself: starting it, gravity, stepping, creation, queries and the event
/// queues.</summary>
/// <remarks>Engine units and axes throughout: centimetres, +X forward, +Y right, +Z up, left-handed.</remarks>
public static class Physics
{
    // ---- World --------------------------------------------------------------------------------------

    /// <summary>Starts the simulation. Idempotent -- a second call while running is a no-op.</summary>
    public static bool Init() => Native.aver_phys_init() != 0;
    /// <summary>Destroys every body and character, then the world.</summary>
    public static void Shutdown() => Native.aver_phys_shutdown();

    /// <summary>Why the last physics call on this thread failed, or <see cref="PhysicsError.Ok"/>.</summary>
    ///
    /// <remarks>WHY THIS IS NOT ON THE CALL ITSELF. Every entry point in the physics ABI returns 1 for
    /// success and 0 for failure, and everything in this assembly tests them as a bool. A negative
    /// code returned from those would be TRUE, silently inverting each of those call sites with no
    /// compile error -- so the reason travels on its own entry point and nothing else changes.
    ///
    /// WHAT IT SEPARATES. <c>Body.Aabb</c> coming back false means the handle is dead
    /// (<see cref="PhysicsError.BadHandle"/>) or there is no world at all
    /// (<see cref="PhysicsError.NotInitialised"/>), and until now nothing told them apart.
    ///
    /// THREAD-LOCAL, which is what makes it safe to read while the physics job pool is running.
    /// Set to Ok on success too, so a stale reason cannot outlive the failure that produced it.
    /// </remarks>
    public static PhysicsError LastError => (PhysicsError)Native.aver_phys_last_error();

    /// <summary>True when the last physics call on this thread recorded a failure. Written as
    /// <c>&lt; 0</c> rather than a switch, so a code added after this build still reads as a failure
    /// rather than falling through to "fine".</summary>
    public static bool LastCallFailed => Native.aver_phys_last_error() < 0;

    /// <summary>True while a world exists. False with physics compiled out.</summary>
    public static bool Ready
    {
        get { try { return Native.aver_phys_ready() != 0; } catch (DllNotFoundException) { return false; } }
    }

    /// <summary>The fixed simulation step, in seconds (1/60 unless set).</summary>
    public static float FixedStep => Native.aver_phys_fixed_step();
    /// <summary>Changes the fixed step. Set it before any bodies exist.</summary>
    public static bool SetFixedStep(float seconds) => Native.aver_phys_set_fixed_step(seconds) != 0;

    /// <summary>Gravity in cm/s^2. Defaults to (0, 0, -980) -- one g, straight down +Z-up.</summary>
    public static void SetGravity(Float3 g) => Native.aver_phys_set_gravity(g.X, g.Y, g.Z);
    public static void SetGravity(float x, float y, float z) => Native.aver_phys_set_gravity(x, y, z);

    /// <summary>Advances by <paramref name="dt"/> seconds of real time, in fixed steps; leftover time
    /// carries and a long stall is clamped. Returns how many fixed steps actually ran.</summary>
    public static int Step(float dt) => Native.aver_phys_step(dt);

    /// <summary>How many bodies are live.</summary>
    public static int BodyCount => Native.aver_phys_body_count();

    // ---- Creation -------------------------------------------------------------------------------------

    /// <summary>A box that never moves -- floors, walls, static level geometry.</summary>
    public static Body AddStaticBox(Float3 centre, Float3 halfExtents) =>
        new(Native.aver_phys_add_static_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z));

    /// <summary>A box that falls and collides. <paramref name="massKg"/> &lt;= 0 derives mass from the volume.</summary>
    public static Body AddDynamicBox(Float3 centre, Float3 halfExtents, float massKg = 0f) =>
        new(Native.aver_phys_add_dynamic_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z, massKg));

    /// <summary>A sphere that falls and collides.</summary>
    public static Body AddDynamicSphere(Float3 centre, float radius, float massKg = 0f) =>
        new(Native.aver_phys_add_dynamic_sphere(centre.X, centre.Y, centre.Z, radius, massKg));

    /// <summary>A box-shaped trigger volume. Reports through <see cref="Overlaps"/>, never pushes anything.</summary>
    public static Body AddSensorBox(Float3 centre, Float3 halfExtents) =>
        new(Native.aver_phys_add_sensor_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z));

    /// <summary>A sphere-shaped trigger volume.</summary>
    public static Body AddSensorSphere(Float3 centre, float radius) =>
        new(Native.aver_phys_add_sensor_sphere(centre.X, centre.Y, centre.Z, radius));

    /// <summary>A convex hull wrapped around <paramref name="points"/>.</summary>
    public static Body AddConvexHull(Float3[] points, Float3 centre, bool dynamic, float massKg = 0f) =>
        new(Native.aver_phys_add_convex_hull(Flatten(points), points.Length, centre.X, centre.Y, centre.Z, dynamic ? 1 : 0, massKg));

    /// <summary>A static-only triangle mesh. <paramref name="indices"/> is 3 per triangle.</summary>
    public static Body AddMesh(Float3[] vertices, int[] indices, Float3 centre) =>
        new(Native.aver_phys_add_mesh(Flatten(vertices), vertices.Length, indices, indices.Length, centre.X, centre.Y, centre.Z));

    /// <summary>A static heightfield: <paramref name="samples"/> is a row-major sampleCount x
    /// sampleCount grid of heights in centimetres, spaced <paramref name="spacingCm"/> apart. Sample
    /// (column x, row y) lands at engine (cx - y*spacingCm, cy + x*spacingCm, cz + height).</summary>
    public static Body AddHeightfield(float[] samples, int sampleCount, float spacingCm, Float3 centre) =>
        new(Native.aver_phys_add_heightfield(samples, sampleCount, spacingCm, centre.X, centre.Y, centre.Z));

    // ---- Soft bodies --------------------------------------------------------------------------------
    // A soft body is a body -- see Body.cs's file comment -- so the handle this returns works with
    // aver_phys_remove_body, SetEntity and every query below unchanged.

    /// <summary>Builds a soft body from a triangle mesh. <paramref name="invMasses"/> is optional (null
    /// = every particle at inverse mass 1); a particle at inverse mass 0 is pinned. <paramref name="compliance"/>
    /// is inverse stiffness (0 = inextensible); <paramref name="pressure"/> inflates a closed mesh from
    /// within (0 for cloth). <paramref name="damping"/>/<paramref name="iterations"/> default to Jolt's
    /// own SoftBodyCreationSettings values.</summary>
    public static Body AddSoftBody(Float3[] vertices, int[] indices, float[]? invMasses, Float3 centre,
                                    float compliance, float pressure, float damping = 0.1f, int iterations = 5) =>
        new(Native.aver_phys_softbody_create(Flatten(vertices), vertices.Length, indices, indices.Length, invMasses,
                                              centre.X, centre.Y, centre.Z, compliance, pressure, damping, iterations));

    /// <summary>As <see cref="AddSoftBody"/>, plus skinned constraints tethering each vertex to where
    /// ordinary bone skinning would place it, free to move up to <paramref name="maxDistanceCm"/> away.
    /// Pass the BIND POSE as <paramref name="vertices"/>; drive the body each frame with
    /// <see cref="Body.SoftBodySkin"/>. <paramref name="jointIndices"/>/<paramref name="jointWeights"/>
    /// are <c>vertices.Length * influences</c> long, matching the mesh's own skinning data.
    /// <paramref name="backStopDistanceCm"/> keeps a vertex from sinking through the surface it hangs
    /// off; negative disables it.</summary>
    public static Body AddSkinnedSoftBody(Float3[] vertices, int[] indices, float[]? invMasses,
                                           int[] jointIndices, float[] jointWeights, int influences, int jointCount,
                                           float maxDistanceCm, float backStopDistanceCm, Float3 centre, float compliance) =>
        new(Native.aver_phys_softbody_create_skinned(Flatten(vertices), vertices.Length, indices, indices.Length, invMasses,
                                                       jointIndices, jointWeights, influences, jointCount,
                                                       maxDistanceCm, backStopDistanceCm, centre.X, centre.Y, centre.Z, compliance));

    private static float[] Flatten(Float3[] points)
    {
        var flat = new float[points.Length * 3];
        for (int i = 0; i < points.Length; i++) { flat[i * 3] = points[i].X; flat[i * 3 + 1] = points[i].Y; flat[i * 3 + 2] = points[i].Z; }
        return flat;
    }

    // ---- Buoyancy -------------------------------------------------------------------------------------
    // The global plane is the important entry point (see physics_abi.h's own file comment on why): a
    // per-body override exists on Body.SetWaterVolume for a puddle or tank at a different height.

    /// <summary>Every dynamic body whose centre is below <paramref name="heightCm"/> gets buoyancy this
    /// step and keeps getting it until the plane is cleared. <paramref name="normal"/> is normally
    /// <see cref="Float3.Up"/>; a non-positive <paramref name="buoyancy"/> disables the plane without
    /// forgetting its other settings.</summary>
    public static void SetWaterPlane(float heightCm, Float3 normal, float buoyancy, float linearDrag, float angularDrag, Float3 fluidVelocity) =>
        Native.aver_phys_set_water_plane(heightCm, normal.ToArray(), buoyancy, linearDrag, angularDrag, fluidVelocity.ToArray());

    /// <summary>Removes the global plane. Bodies stop being held up on the next step.</summary>
    public static void ClearWaterPlane() => Native.aver_phys_clear_water_plane();

    /// <summary>The global plane's height, or null while none is set. Keep the RENDERED water surface
    /// reading from this rather than a second stored height, or the two will drift.</summary>
    public static float? WaterPlane()
    {
        var h = new float[1];
        return Native.aver_phys_water_plane(h) != 0 ? h[0] : null;
    }

    /// <summary>How many bodies had buoyancy applied on the last step -- tells "nothing floats" apart
    /// from "nothing is in the water" without a debugger.</summary>
    public static int BuoyantBodyCount => Native.aver_phys_buoyant_body_count();

    // ---- Collision layers -----------------------------------------------------------------------------
    // Layer 0 is the default and collides with everything; a project that never touches this behaves
    // exactly as if the header did not exist. See physics_layers_abi.h's own file comment for how a
    // layer differs from the moving/static split.

    public const int LayerCount = 16;
    /// <summary>Every layer -- passing this to a filtered query makes it identical to the unfiltered one.</summary>
    public const uint LayerMaskAll = 0xFFFFFFFFu;
    /// <summary>The bit for one layer, for building a mask: <c>LayerBit(2) | LayerBit(5)</c>.</summary>
    public static uint LayerBit(int layer) => 1u << layer;

    /// <summary>Whether two layers collide. SYMMETRIC -- setting (a, b) also sets (b, a). Every pair
    /// starts enabled; this is a subtractive API, so a call says what does NOT collide.</summary>
    public static bool SetLayerCollision(int layerA, int layerB, bool enabled) => Native.aver_phys_set_layer_collision(layerA, layerB, enabled ? 1 : 0) != 0;
    public static bool LayerCollision(int layerA, int layerB) => Native.aver_phys_layer_collision(layerA, layerB) != 0;
    /// <summary>Puts every pair back to colliding -- what a level teardown wants, since the matrix
    /// belongs to the world and must not leak into the next level.</summary>
    public static void ResetLayerCollisions() => Native.aver_phys_reset_layer_collisions();

    // ---- Contact and overlap events ------------------------------------------------------------------
    // POLLED, not called back. Both queues are cleared at the start of each Step.

    /// <summary>Contacts that began during the last step.</summary>
    public static IReadOnlyList<ContactEvent> Contacts
    {
        get
        {
            int n = Native.aver_phys_contact_count();
            var list = new List<ContactEvent>(n);
            var p = new float[3]; var nrm = new float[3];
            for (int i = 0; i < n; i++)
                if (Native.aver_phys_contact_get(i, out int a, out int b, p, nrm) != 0)
                    list.Add(new ContactEvent(a, b, new Float3(p[0], p[1], p[2]), new Float3(nrm[0], nrm[1], nrm[2])));
            return list;
        }
    }

    /// <summary>Sensor volumes entered or left during the last step.</summary>
    public static IReadOnlyList<OverlapEvent> Overlaps
    {
        get
        {
            int n = Native.aver_phys_overlap_count();
            var list = new List<OverlapEvent>(n);
            for (int i = 0; i < n; i++)
                if (Native.aver_phys_overlap_get(i, out int s, out int b, out int entered) != 0)
                    list.Add(new OverlapEvent(s, b, entered != 0));
            return list;
        }
    }

    // ---- Queries -------------------------------------------------------------------------------------
    // Jolt broadphase queries are not deterministic between equidistant bodies.

    /// <summary>Casts a ray up to <paramref name="maxDistanceCm"/> and returns the first hit.</summary>
    public static RaycastHit Raycast(Float3 origin, Float3 direction, float maxDistanceCm)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_raycast(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z,
                                             maxDistanceCm, p, n, out int entity);
        return body == 0 ? default : new RaycastHit(body, entity, new Float3(p[0], p[1], p[2]), new Float3(n[0], n[1], n[2]));
    }

    /// <summary>True if anything lies within <paramref name="maxDistanceCm"/> along the ray.</summary>
    public static bool RaycastAny(Float3 origin, Float3 direction, float maxDistanceCm) => Raycast(origin, direction, maxDistanceCm).Hit;

    /// <summary>As <see cref="Raycast"/>, but only hitting bodies on a layer in <paramref name="layerMask"/>
    /// and never <paramref name="ignoreBody"/> -- almost always the body doing the casting, so a ray fired
    /// from inside your own capsule does not hit yourself at distance 0.</summary>
    public static RaycastHit RaycastEx(Float3 origin, Float3 direction, float maxDistanceCm, uint layerMask, Body ignoreBody)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_raycast_ex(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z,
                                                maxDistanceCm, layerMask, ignoreBody.Handle, p, n, out int entity);
        return body == 0 ? default : new RaycastHit(body, entity, new Float3(p[0], p[1], p[2]), new Float3(n[0], n[1], n[2]));
    }

    /// <summary>Every body overlapping a sphere. A returned count equal to <paramref name="maxResults"/>
    /// means truncated.</summary>
    public static Body[] OverlapSphere(Float3 centre, float radius, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        var raw = new int[maxResults];
        int n = Native.aver_phys_overlap_sphere(centre.X, centre.Y, centre.Z, radius, raw, maxResults);
        var outv = new Body[n];
        for (int i = 0; i < n; i++) outv[i] = new Body(raw[i]);
        return outv;
    }

    /// <summary>As <see cref="OverlapSphere"/>, filtered to a layer mask and skipping <paramref name="ignoreBody"/>.</summary>
    public static Body[] OverlapSphereEx(Float3 centre, float radius, uint layerMask, Body ignoreBody, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        var raw = new int[maxResults];
        int n = Native.aver_phys_overlap_sphere_ex(centre.X, centre.Y, centre.Z, radius, layerMask, ignoreBody.Handle, raw, maxResults);
        var outv = new Body[n];
        for (int i = 0; i < n; i++) outv[i] = new Body(raw[i]);
        return outv;
    }

    /// <summary>Sweeps a sphere and returns the first thing it touches. <see cref="RaycastHit.Entity"/>
    /// is always 0 here -- the native sweep does not resolve entities yet.</summary>
    public static RaycastHit SphereCast(Float3 origin, Float3 direction, float maxDistanceCm, float radius)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_sphere_cast(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z,
                                                 maxDistanceCm, radius, p, n);
        return body == 0 ? default : new RaycastHit(body, 0, new Float3(p[0], p[1], p[2]), new Float3(n[0], n[1], n[2]));
    }

    /// <summary>As <see cref="SphereCast"/>, filtered to a layer mask and skipping <paramref name="ignoreBody"/>.</summary>
    public static RaycastHit SphereCastEx(Float3 origin, Float3 direction, float maxDistanceCm, float radius, uint layerMask, Body ignoreBody)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_sphere_cast_ex(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z,
                                                    maxDistanceCm, radius, layerMask, ignoreBody.Handle, p, n);
        return body == 0 ? default : new RaycastHit(body, 0, new Float3(p[0], p[1], p[2]), new Float3(n[0], n[1], n[2]));
    }
}
