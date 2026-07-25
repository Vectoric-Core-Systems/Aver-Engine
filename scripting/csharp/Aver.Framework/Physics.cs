using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>P/Invoke into Aver.Physics. Internal: scripts use <see cref="Physics"/>.</summary>
internal static class Phys
{
    private const string Lib = "Aver.Physics";

    [DllImport(Lib)] internal static extern int   aver_phys_ready();
    [DllImport(Lib)] internal static extern void  aver_phys_set_gravity(float x, float y, float z);
    [DllImport(Lib)] internal static extern float aver_phys_fixed_step();

    [DllImport(Lib)] internal static extern int aver_phys_add_static_box(float cx, float cy, float cz, float hx, float hy, float hz);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_box(float cx, float cy, float cz, float hx, float hy, float hz, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_sphere(float cx, float cy, float cz, float radius, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_remove_body(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_position(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_rotation(int body, float[] outQuat);
    [DllImport(Lib)] internal static extern int aver_phys_body_velocity(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_position(int body, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_velocity(int body, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_body_count();

    [DllImport(Lib)] internal static extern int aver_phys_character_create(float radius, float height, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_character_destroy(int ch);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_velocity(int ch, float vx, float vy, float vz);
    [DllImport(Lib)] internal static extern int aver_phys_character_velocity(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_position(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_position(int ch, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_character_grounded(int ch);

    [DllImport(Lib)] internal static extern int aver_phys_raycast(float ox, float oy, float oz,
                                                                 float dx, float dy, float dz,
                                                                 float maxDistCm, float[] outPoint, float[] outNormal);

    [DllImport(Lib)] internal static extern int aver_phys_add_sensor_box(float cx, float cy, float cz, float hx, float hy, float hz);
    [DllImport(Lib)] internal static extern int aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);

    [DllImport(Lib)] internal static extern int aver_phys_contact_count();
    [DllImport(Lib)] internal static extern int aver_phys_contact_get(int index, out int outA, out int outB, float[] outPoint, float[] outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_overlap_count();
    [DllImport(Lib)] internal static extern int aver_phys_overlap_get(int index, out int outSensor, out int outBody, out int outEntered);

    [DllImport(Lib)] internal static extern int aver_phys_overlap_sphere(float x, float y, float z, float radius, int[] outBodies, int maxBodies);
    [DllImport(Lib)] internal static extern int aver_phys_sphere_cast(float ox, float oy, float oz,
                                                                     float dx, float dy, float dz,
                                                                     float maxDistCm, float radius,
                                                                     float[] outPoint, float[] outNormal);
}

/// <summary>Two solid bodies that began touching during the last step.</summary>
public readonly struct ContactEvent
{
    /// <summary>The two bodies involved. Which is which carries no meaning — a contact is symmetric.</summary>
    public Body A { get; }
    public Body B { get; }
    /// <summary>Where they touched, centimetres.</summary>
    public Vec3 Point { get; }
    /// <summary>The contact normal, unit length.</summary>
    public Vec3 Normal { get; }

    internal ContactEvent(int a, int b, Vec3 point, Vec3 normal)
    { A = new Body(a); B = new Body(b); Point = point; Normal = normal; }

    /// <summary>The other body in this contact, given one of them; invalid if <paramref name="one"/> is neither.</summary>
    public Body Other(Body one) => one.Handle == A.Handle ? B : one.Handle == B.Handle ? A : Body.None;
}

/// <summary>Something entering or leaving a sensor volume during the last step.</summary>
public readonly struct OverlapEvent
{
    /// <summary>The sensor whose volume it is.</summary>
    public Body Sensor { get; }
    /// <summary>What entered or left.</summary>
    public Body Other { get; }
    /// <summary>True on entering, false on leaving.</summary>
    public bool Entered { get; }

    internal OverlapEvent(int sensor, int other, bool entered)
    { Sensor = new Body(sensor); Other = new Body(other); Entered = entered; }
}

/// <summary>What a <see cref="Physics.Raycast(Vec3, Vec3, float)"/> found.</summary>
public readonly struct RaycastHit
{
    /// <summary>
    /// The body that was hit; <see cref="Framework.Body.None"/> when the ray hit nothing.
    /// </summary>
    /// <remarks>
    /// A <see cref="Framework.Body"/> rather than a raw handle, so what comes out of a query is the
    /// same thing the rest of the API takes in. Handing back an int forced callers to reconstruct a
    /// Body from it, which they cannot do — the constructor is the framework's.
    /// </remarks>
    public Body Body { get; }
    /// <summary>Where the ray met the surface, in centimetres.</summary>
    public Vec3 Point { get; }
    /// <summary>The surface normal at that point, unit length.</summary>
    public Vec3 Normal { get; }
    /// <summary>True when the ray hit something. Check this before reading the rest.</summary>
    public bool Hit => Body.IsValid;

    internal RaycastHit(int body, Vec3 point, Vec3 normal)
    { Body = new Body(body); Point = point; Normal = normal; }
}

/// <summary>
/// A rigid body in the simulation. A handle, not an object: 0 is invalid, and the body it names
/// lives natively.
/// </summary>
public readonly struct Body : IEquatable<Body>
{
    /// <summary>The raw ABI handle. 0 is invalid.</summary>
    public int Handle { get; }
    internal Body(int h) { Handle = h; }

    /// <summary>The invalid handle.</summary>
    public static Body None => new(0);
    /// <summary>True when this names a body rather than nothing.</summary>
    public bool IsValid => Handle != 0;

    /// <summary>Centre of mass position, centimetres. <see cref="Vec3.Zero"/> for a dead handle.</summary>
    public Vec3 Position
    {
        get
        {
            float[] v = new float[3];
            return Phys.aver_phys_body_position(Handle, v) != 0 ? new Vec3(v[0], v[1], v[2]) : Vec3.Zero;
        }
    }

    /// <summary>Rotation. <see cref="Quat.Identity"/> for a dead handle.</summary>
    public Quat Rotation
    {
        get
        {
            float[] q = new float[4];
            return Phys.aver_phys_body_rotation(Handle, q) != 0 ? new Quat(q[0], q[1], q[2], q[3]) : Quat.Identity;
        }
    }

    /// <summary>Linear velocity, cm/s.</summary>
    public Vec3 Velocity
    {
        get
        {
            float[] v = new float[3];
            return Phys.aver_phys_body_velocity(Handle, v) != 0 ? new Vec3(v[0], v[1], v[2]) : Vec3.Zero;
        }
    }

    /// <summary>Teleport the body. Use sparingly on a dynamic body — it ignores collision on the way.</summary>
    public bool SetPosition(Vec3 p) => Phys.aver_phys_body_set_position(Handle, p.X, p.Y, p.Z) != 0;
    /// <summary>Set linear velocity, cm/s.</summary>
    public bool SetVelocity(Vec3 v) => Phys.aver_phys_body_set_velocity(Handle, v.X, v.Y, v.Z) != 0;

    /// <summary>
    /// Add to the body's velocity — a nudge rather than a teleport, which is what a bullet, an
    /// explosion or a bump wants.
    /// </summary>
    /// <remarks>
    /// A velocity change, not a true impulse: it ignores mass, so a heavy body moves as readily as a
    /// light one. Real impulse handling needs mass out of the ABI; until then this is honest about
    /// being a kick rather than pretending to be Newtonian.
    /// </remarks>
    public bool AddVelocity(Vec3 delta) => SetVelocity(Velocity + delta);
    /// <summary>Remove the body from the world. The handle is dead afterwards.</summary>
    public bool Destroy() => Phys.aver_phys_remove_body(Handle) != 0;

    public bool Equals(Body o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is Body b && Equals(b);
    public override int GetHashCode() => Handle;
    public override string ToString() => IsValid ? $"Body#{Handle}" : "Body.None";
}

/// <summary>
/// The simulation, as a script sees it: rigid bodies, raycasts and gravity.
/// </summary>
/// <remarks>
/// Everything here is in the ENGINE's contract — centimetres, +X forward, +Y right, +Z up,
/// left-handed. The backend (Jolt) is right-handed, +Y up and metric; that translation happens once
/// behind the native ABI, so nothing above ever sees a Jolt convention.
///
/// The world is stepped by the frame loop between the PrePhysics and PostPhysics tick groups, at a
/// FIXED step (<see cref="FixedStep"/>) regardless of frame rate. So set what you want in a
/// PrePhysics tick and read what happened in a PostPhysics one.
/// </remarks>
public static class Physics
{
    /// <summary>True when the simulation is running. False in a build with physics compiled out.</summary>
    public static bool Ready
    {
        get { try { return Phys.aver_phys_ready() != 0; } catch (DllNotFoundException) { return false; } }
    }

    /// <summary>The fixed simulation step in seconds — 1/60 unless the host changed it.</summary>
    public static float FixedStep => Phys.aver_phys_fixed_step();

    /// <summary>Set gravity in cm/s². The default is (0, 0, -980): one g, straight down.</summary>
    public static void SetGravity(Vec3 g) => Phys.aver_phys_set_gravity(g.X, g.Y, g.Z);

    /// <summary>How many bodies exist. Mostly useful for a test to assert on.</summary>
    public static int BodyCount => Phys.aver_phys_body_count();

    /// <summary>A box that never moves. Centre and half-extents in centimetres.</summary>
    public static Body AddStaticBox(Vec3 centre, Vec3 halfExtents) =>
        new(Phys.aver_phys_add_static_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z));

    /// <summary>A box that falls and collides. <paramref name="massKg"/> &lt;= 0 derives mass from the volume.</summary>
    public static Body AddDynamicBox(Vec3 centre, Vec3 halfExtents, float massKg = 0f) =>
        new(Phys.aver_phys_add_dynamic_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z, massKg));

    /// <summary>A sphere that falls and collides. Radius in centimetres.</summary>
    public static Body AddDynamicSphere(Vec3 centre, float radius, float massKg = 0f) =>
        new(Phys.aver_phys_add_dynamic_sphere(centre.X, centre.Y, centre.Z, radius, massKg));

    /// <summary>
    /// Cast a ray from <paramref name="origin"/> along <paramref name="direction"/> for at most
    /// <paramref name="maxDistanceCm"/> centimetres.
    /// </summary>
    /// <remarks>
    /// The direction does not need to be unit length. Note that the backend documents broadphase
    /// queries as non-deterministic across runs: rely on whether something was hit and where, not on
    /// WHICH of several equidistant bodies comes back.
    /// </remarks>
    public static RaycastHit Raycast(Vec3 origin, Vec3 direction, float maxDistanceCm)
    {
        float[] p = new float[3], n = new float[3];
        int body = Phys.aver_phys_raycast(origin.X, origin.Y, origin.Z,
                                          direction.X, direction.Y, direction.Z, maxDistanceCm, p, n);
        return body == 0 ? default : new RaycastHit(body, new Vec3(p[0], p[1], p[2]), new Vec3(n[0], n[1], n[2]));
    }

    /// <summary>True if anything lies within <paramref name="maxDistanceCm"/> along the ray.</summary>
    public static bool RaycastAny(Vec3 origin, Vec3 direction, float maxDistanceCm) =>
        Raycast(origin, direction, maxDistanceCm).Hit;

    /// <summary>
    /// A trigger volume: it notices what enters and leaves, and pushes nothing. Report through
    /// <see cref="Overlaps"/>.
    /// </summary>
    public static Body AddSensorBox(Vec3 centre, Vec3 halfExtents) =>
        new(Phys.aver_phys_add_sensor_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z));

    /// <summary>A spherical trigger volume.</summary>
    public static Body AddSensorSphere(Vec3 centre, float radius) =>
        new(Phys.aver_phys_add_sensor_sphere(centre.X, centre.Y, centre.Z, radius));

    /// <summary>
    /// Contacts that began during the last step. Read them from a <c>PostPhysics</c> tick.
    /// </summary>
    /// <remarks>
    /// Polled rather than delivered by callback: the backend finds contacts on several worker threads
    /// at once, in an order it does not guarantee, so a callback would arrive off the main thread in
    /// the middle of a simulation step. Draining a queue afterwards keeps every reaction on one
    /// thread, in a fixed order, at a moment when changing the world is safe.
    ///
    /// <para>The queue is emptied at the start of each step, so a tick that does not read it misses
    /// those contacts — which is the right trade for gameplay, where a stale contact is worse than a
    /// dropped one.</para>
    /// </remarks>
    public static IReadOnlyList<ContactEvent> Contacts
    {
        get
        {
            int n = Phys.aver_phys_contact_count();
            var list = new List<ContactEvent>(n);
            float[] p = new float[3], nrm = new float[3];
            for (int i = 0; i < n; i++)
                if (Phys.aver_phys_contact_get(i, out int a, out int b, p, nrm) != 0)
                    list.Add(new ContactEvent(a, b, new Vec3(p[0], p[1], p[2]), new Vec3(nrm[0], nrm[1], nrm[2])));
            return list;
        }
    }

    /// <summary>Sensor volumes entered or left during the last step. Same polling rules as <see cref="Contacts"/>.</summary>
    public static IReadOnlyList<OverlapEvent> Overlaps
    {
        get
        {
            int n = Phys.aver_phys_overlap_count();
            var list = new List<OverlapEvent>(n);
            for (int i = 0; i < n; i++)
                if (Phys.aver_phys_overlap_get(i, out int s, out int b, out int entered) != 0)
                    list.Add(new OverlapEvent(s, b, entered != 0));
            return list;
        }
    }

    /// <summary>
    /// Every body overlapping a sphere — "what is within blast radius", "what is on the pressure plate".
    /// </summary>
    /// <remarks>
    /// <paramref name="maxResults"/> bounds the answer. If the count comes back EQUAL to it the list
    /// was truncated and there may be more; ask again with a larger bound rather than assume you saw
    /// everything.
    /// </remarks>
    public static Body[] OverlapSphere(Vec3 centre, float radius, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        int[] raw = new int[maxResults];
        int n = Phys.aver_phys_overlap_sphere(centre.X, centre.Y, centre.Z, radius, raw, maxResults);
        var outv = new Body[n];
        for (int i = 0; i < n; i++) outv[i] = new Body(raw[i]);
        return outv;
    }

    /// <summary>
    /// Sweep a sphere and report the first thing it touches. Unlike a ray this has THICKNESS, so it
    /// is what a projectile, a camera boom or a step-up probe wants — a ray slips through gaps a
    /// moving object could never fit through.
    /// </summary>
    public static RaycastHit SphereCast(Vec3 origin, Vec3 direction, float maxDistanceCm, float radius)
    {
        float[] p = new float[3], n = new float[3];
        int body = Phys.aver_phys_sphere_cast(origin.X, origin.Y, origin.Z,
                                              direction.X, direction.Y, direction.Z,
                                              maxDistanceCm, radius, p, n);
        return body == 0 ? default : new RaycastHit(body, new Vec3(p[0], p[1], p[2]), new Vec3(n[0], n[1], n[2]));
    }
}
