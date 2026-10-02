// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// Script-facing physics: bodies, sensors, queries, and the contact/overlap event queues.
//
// THE PUBLIC SURFACE BELOW (ContactEvent/OverlapEvent/RaycastHit/Body/Physics) IS NOW A SHIM OVER
// Aver.Physics, not a second P/Invoke surface: every aver_phys_* export it needs is already bound in
// Aver.Physics/Native.cs, the file tests/abi/src/AbiEnumTest.cpp checks character for character
// against the C headers, so this file only converts between the engine-wide Vec3/Quat/Entity types
// scripts already use and Aver.Physics's own Float3/Quaternion/int (which Aver.Physics cannot use
// itself: it is a leaf with no reference to Aver.Scene or Aver.Framework).
//
// THE Phys CLASS BELOW STILL HOLDS A SMALL, DIRECT DllImport SET, and that is not an oversight: it is
// what Character.cs calls into for the capsule it drives every tick (aver_phys_character_* and
// aver_phys_set_entity), and Character.cs is outside this change's one file. Duplicating those nine
// bindings past Aver.Physics.CharacterBody costs nothing at runtime -- both DllImports resolve to the
// same native export -- and rewiring Character.cs's call sites belongs to whoever owns that file.
//
// Physics beyond what shipped here before 0.5.0 -- joints, the extra rigid-body shapes, collision
// layers, soft bodies, buoyancy, the rest of the character surface -- lives directly on
// Aver.Physics.Joint / .Shapes / .Physics / .Body / .CharacterBody. A script can already `using
// Aver.Physics;` for those; nothing about that needs a Vec3-flavoured wrapper repeated here, and
// duplicating the whole surface a second time under Aver.Framework would be the same mistake this
// file's own existence warns against, one namespace up.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Aver.Scene;
using AP = Aver.Physics;

namespace Aver.Framework;

/// <summary>P/Invoke for the handful of calls Character.cs makes directly. See this file's own header
/// comment for why these nine are not routed through Aver.Physics.CharacterBody instead.</summary>
internal static class Phys
{
    private const string Lib = "Aver.Physics";

    [DllImport(Lib)] internal static extern int aver_phys_character_create(float radius, float height, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_character_destroy(int ch);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_velocity(int ch, float vx, float vy, float vz);
    [DllImport(Lib)] internal static extern int aver_phys_character_velocity(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_position(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_position(int ch, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_character_grounded(int ch);
    [DllImport(Lib)] internal static extern int aver_phys_set_entity(int handle, int entity);
    // The Velocity setter's dead stop also clears what a moving ground lent the character.
    [DllImport(Lib)] internal static extern int aver_phys_character_set_inherited_velocity(int ch, float x, float y, float z);
}

/// <summary>Two solid bodies that began touching during the last step.</summary>
public readonly struct ContactEvent
{
    /// <summary>One of the two bodies; a contact is symmetric.</summary>
    public Body A { get; }
    public Body B { get; }
    /// <summary>Where they touched, centimetres.</summary>
    public Vec3 Point { get; }
    /// <summary>The contact normal, unit length.</summary>
    public Vec3 Normal { get; }

    internal ContactEvent(AP.ContactEvent c)
    { A = new Body(c.A.Handle); B = new Body(c.B.Handle); Point = Physics.ToVec3(c.Point); Normal = Physics.ToVec3(c.Normal); }

    /// <summary>The other body in this contact; invalid if <paramref name="one"/> is neither.</summary>
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

    internal OverlapEvent(AP.OverlapEvent o)
    { Sensor = new Body(o.Sensor.Handle); Other = new Body(o.Other.Handle); Entered = o.Entered; }
}

/// <summary>What a raycast or sphere cast found.</summary>
public readonly struct RaycastHit
{
    /// <summary>The body that was hit; <see cref="Framework.Body.None"/> when nothing was hit.</summary>
    public Body Body { get; }
    /// <summary>The scene entity that owns the hit body or character, stamped there by whoever created
    /// it (see <c>aver_phys_set_entity</c>). <see cref="Framework.Entity.None"/> (0) means the hit is
    /// real but nothing claimed it -- a landscape heightfield today -- which is NOT the same thing as
    /// <see cref="Hit"/> being false; check <see cref="Hit"/> first. Only populated by
    /// <see cref="Physics.Raycast"/> -- always <see cref="Framework.Entity.None"/> from
    /// <see cref="Physics.SphereCast"/>, which does not resolve entities yet.</summary>
    public Entity Entity { get; }
    /// <summary>Where the ray met the surface, in centimetres.</summary>
    public Vec3 Point { get; }
    /// <summary>The surface normal at that point, unit length.</summary>
    public Vec3 Normal { get; }
    /// <summary>True when the ray hit something. Check this before reading the rest.</summary>
    public bool Hit => Body.IsValid;

    internal RaycastHit(AP.RaycastHit h)
    { Body = new Body(h.Body.Handle); Entity = new Entity(h.Entity); Point = Physics.ToVec3(h.Point); Normal = Physics.ToVec3(h.Normal); }
}

/// <summary>A rigid body in the simulation. A handle, not an object: 0 is invalid.</summary>
public readonly struct Body : IEquatable<Body>
{
    /// <summary>The raw ABI handle. 0 is invalid.</summary>
    public int Handle { get; }
    internal Body(int h) { Handle = h; }

    /// <summary>The invalid handle.</summary>
    public static Body None => new(0);
    /// <summary>True when this names a body rather than nothing.</summary>
    public bool IsValid => Handle != 0;

    private AP.Body Native => new(Handle);

    /// <summary>Centre of mass position, centimetres. Zero for a dead handle.</summary>
    public Vec3 Position => Physics.ToVec3(Native.Position);

    /// <summary>Rotation. Identity for a dead handle.</summary>
    public Quat Rotation => Physics.ToQuat(Native.Rotation);

    /// <summary>Linear velocity, cm/s.</summary>
    public Vec3 Velocity => Physics.ToVec3(Native.Velocity);

    /// <summary>Teleports the body, ignoring collision on the way.</summary>
    public bool SetPosition(Vec3 p) => Native.SetPosition(p.X, p.Y, p.Z);
    /// <summary>Sets linear velocity, cm/s.</summary>
    public bool SetVelocity(Vec3 v) => Native.SetVelocity(v.X, v.Y, v.Z);

    /// <summary>Adds to the body's velocity. A velocity change, not a mass-scaled impulse.</summary>
    public bool AddVelocity(Vec3 delta) => Native.AddVelocity(Physics.ToFloat3(delta));
    /// <summary>Removes the body from the world. The handle is dead afterwards.</summary>
    public bool Destroy() => Native.Destroy();

    /// <summary>Stamps this body with the entity that owns it, so a later <see cref="Physics.Raycast"/>
    /// against it reports <paramref name="e"/> through <see cref="RaycastHit.Entity"/>. Bodies made by
    /// a level placement, a character, or the editor already have this set; a script creating a body
    /// of its own (<see cref="Physics.AddDynamicBox"/> and siblings) is the case this is for.</summary>
    public bool SetEntity(Entity e) => Native.SetEntity(e.Handle);

    public bool Equals(Body o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is Body b && Equals(b);
    public override int GetHashCode() => Handle;
    public override string ToString() => IsValid ? $"Body#{Handle}" : "Body.None";
}

/// <summary>The simulation as a script sees it: bodies, sensors, queries and gravity.</summary>
/// <remarks>Engine units throughout: centimetres, +X forward, +Y right, +Z up, left-handed.
///
/// This is the subset that predates Aver.Physics and keeps the Vec3-flavoured surface existing scripts
/// already call. Joints, soft bodies, buoyancy, collision layers, the extra rigid-body shapes and the
/// rest of the character surface are reached directly through <c>Aver.Physics</c> -- see this file's
/// own header comment for why that is not duplicated here too.</remarks>
public static class Physics
{
    /// <summary>True when the simulation is running. False with physics compiled out.</summary>
    public static bool Ready => AP.Physics.Ready;

    /// <summary>The fixed simulation step in seconds.</summary>
    public static float FixedStep => AP.Physics.FixedStep;

    /// <summary>Sets gravity in cm/s². The default is (0, 0, -980).</summary>
    public static void SetGravity(Vec3 g) => AP.Physics.SetGravity(g.X, g.Y, g.Z);

    /// <summary>How many bodies exist.</summary>
    public static int BodyCount => AP.Physics.BodyCount;

    /// <summary>Adds a box that never moves. Centre and half-extents in centimetres.</summary>
    public static Body AddStaticBox(Vec3 centre, Vec3 halfExtents) =>
        new(AP.Physics.AddStaticBox(ToFloat3(centre), ToFloat3(halfExtents)).Handle);

    /// <summary>Adds a box that falls and collides. <paramref name="massKg"/> &lt;= 0 derives mass from the volume.</summary>
    public static Body AddDynamicBox(Vec3 centre, Vec3 halfExtents, float massKg = 0f) =>
        new(AP.Physics.AddDynamicBox(ToFloat3(centre), ToFloat3(halfExtents), massKg).Handle);

    /// <summary>Adds a sphere that falls and collides. Radius in centimetres.</summary>
    public static Body AddDynamicSphere(Vec3 centre, float radius, float massKg = 0f) =>
        new(AP.Physics.AddDynamicSphere(ToFloat3(centre), radius, massKg).Handle);

    /// <summary>Casts a ray up to <paramref name="maxDistanceCm"/> and returns the first hit, including
    /// which entity (if any) owns whatever it hit -- see <see cref="RaycastHit.Entity"/>.</summary>
    public static RaycastHit Raycast(Vec3 origin, Vec3 direction, float maxDistanceCm) =>
        new(AP.Physics.Raycast(ToFloat3(origin), ToFloat3(direction), maxDistanceCm));

    /// <summary>True if anything lies within <paramref name="maxDistanceCm"/> along the ray.</summary>
    public static bool RaycastAny(Vec3 origin, Vec3 direction, float maxDistanceCm) =>
        Raycast(origin, direction, maxDistanceCm).Hit;

    /// <summary>Adds a box trigger volume. Reports through <see cref="Overlaps"/>.</summary>
    public static Body AddSensorBox(Vec3 centre, Vec3 halfExtents) =>
        new(AP.Physics.AddSensorBox(ToFloat3(centre), ToFloat3(halfExtents)).Handle);

    /// <summary>Adds a spherical trigger volume.</summary>
    public static Body AddSensorSphere(Vec3 centre, float radius) =>
        new(AP.Physics.AddSensorSphere(ToFloat3(centre), radius).Handle);

    /// <summary>Contacts that began during the last step. Drained each step; read from a PostPhysics tick.</summary>
    public static IReadOnlyList<ContactEvent> Contacts
    {
        get
        {
            var src = AP.Physics.Contacts;
            var list = new List<ContactEvent>(src.Count);
            foreach (var c in src) list.Add(new ContactEvent(c));
            return list;
        }
    }

    /// <summary>Sensor volumes entered or left during the last step.</summary>
    public static IReadOnlyList<OverlapEvent> Overlaps
    {
        get
        {
            var src = AP.Physics.Overlaps;
            var list = new List<OverlapEvent>(src.Count);
            foreach (var o in src) list.Add(new OverlapEvent(o));
            return list;
        }
    }

    /// <summary>Every body overlapping a sphere. A count equal to <paramref name="maxResults"/> means truncated.</summary>
    public static Body[] OverlapSphere(Vec3 centre, float radius, int maxResults = 64)
    {
        var src = AP.Physics.OverlapSphere(ToFloat3(centre), radius, maxResults);
        var outv = new Body[src.Length];
        for (int i = 0; i < src.Length; i++) outv[i] = new Body(src[i].Handle);
        return outv;
    }

    /// <summary>Sweeps a sphere and returns the first thing it touches. Unlike <see cref="Raycast"/>,
    /// <see cref="RaycastHit.Entity"/> is always <see cref="Framework.Entity.None"/> here -- the
    /// native sweep does not resolve entities yet, so treat it as unknown rather than "unowned".</summary>
    public static RaycastHit SphereCast(Vec3 origin, Vec3 direction, float maxDistanceCm, float radius) =>
        new(AP.Physics.SphereCast(ToFloat3(origin), ToFloat3(direction), maxDistanceCm, radius));

    // ---- Vec3/Quat <-> Float3/Quaternion, the only thing this file adds over Aver.Physics itself ------

    internal static AP.Float3 ToFloat3(Vec3 v) => new(v.X, v.Y, v.Z);
    internal static Vec3 ToVec3(AP.Float3 v) => new(v.X, v.Y, v.Z);
    internal static Quat ToQuat(AP.Quaternion q) => new(q.X, q.Y, q.Z, q.W);
}
