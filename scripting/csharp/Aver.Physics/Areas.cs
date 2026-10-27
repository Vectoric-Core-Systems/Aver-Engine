// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Godot's contact_monitor and Area3D on Aver.Physics (G2). The native side is physics_areas_abi.h; docs/physics/GODOT_AREAS.md has the
// rules. Nothing here is on by default: a body is not monitored and a sensor is not an area until asked, and no Godot default is built
// in (the converter or Aver.Fabric passes monitoring, monitorable, the masks, the modes and the numbers).

using System.Collections.Generic;

namespace Aver.Physics;

/// <summary>How an area treats gravity or damping, in priority order. Values are the header's AVER_PHYS_AREA_OVERRIDE_*.</summary>
public enum AreaOverrideMode
{
    /// <summary>The area does not touch the property.</summary>
    Disabled = 0,
    /// <summary>Adds to what higher-priority areas gave and keeps going.</summary>
    Combine = 1,
    /// <summary>Adds, then stops: lower-priority areas and the world are ignored.</summary>
    CombineReplace = 2,
    /// <summary>Replaces what was gathered and stops.</summary>
    Replace = 3,
    /// <summary>Replaces what was gathered and keeps going.</summary>
    ReplaceCombine = 4,
}

/// <summary>A monitored body started or stopped touching another body (Godot body_entered / body_exited).</summary>
public readonly struct BodyContactEvent
{
    /// <summary>The monitored body.</summary>
    public Body Body { get; }
    /// <summary>Its partner. May be dead (an END after the partner was removed).</summary>
    public Body Other { get; }
    /// <summary>True for BEGIN, false for END.</summary>
    public bool Began { get; }
    /// <summary>Where the pair first touched, centimetres, world space.</summary>
    public Float3 Point { get; }
    /// <summary>Unit normal from <see cref="Body"/> to <see cref="Other"/> at the first touch.</summary>
    public Float3 Normal { get; }
    internal BodyContactEvent(int body, int other, bool began, Float3 point, Float3 normal)
    { Body = new Body(body); Other = new Body(other); Began = began; Point = point; Normal = normal; }
}

/// <summary>A body or another area entered or left a monitoring area (Godot body_entered / area_entered and the exits).</summary>
public readonly struct AreaEvent
{
    public Area Area { get; }
    /// <summary>The body or area handle; check <see cref="OtherIsArea"/>.</summary>
    public int Other { get; }
    public bool Entered { get; }
    public bool OtherIsArea { get; }
    internal AreaEvent(int area, int other, bool entered, bool otherIsArea)
    { Area = new Area(area); Other = other; Entered = entered; OtherIsArea = otherIsArea; }
}

public readonly partial struct Body
{
    /// <summary>Starts or stops monitoring this body's contacts (Godot contact_monitor with max_contacts_reported). Up to
    /// <paramref name="maxContacts"/> partners are tracked; see <see cref="Physics.BodyContactEvents"/>.</summary>
    public bool SetContactMonitor(bool on, int maxContacts = 0) => Native.aver_phys_body_set_contact_monitor(Handle, on ? 1 : 0, maxContacts) != 0;

    /// <summary>True while the body is contact-monitored.</summary>
    public bool ContactMonitored => Native.aver_phys_body_contact_monitor(Handle, out _) != 0;

    /// <summary>The bodies this monitored body touches now (ascending handle, at most max_contacts); empty if unmonitored.</summary>
    public IReadOnlyList<Body> Contacts
    {
        get
        {
            int n = Native.aver_phys_body_contact_count(Handle);
            var list = new List<Body>(n);
            for (int i = 0; i < n; i++)
                if (Native.aver_phys_body_contact_get(Handle, i, out int other, null, null) != 0) list.Add(new Body(other));
            return list;
        }
    }

    /// <summary>Where a tracked touch began and its normal (from this body to <paramref name="other"/>); false if not a tracked partner.</summary>
    public bool TryGetContact(Body other, out Float3 point, out Float3 normal)
    {
        point = Float3.Zero; normal = Float3.Zero;
        int n = Native.aver_phys_body_contact_count(Handle);
        var p = new float[3]; var nrm = new float[3];
        for (int i = 0; i < n; i++)
            if (Native.aver_phys_body_contact_get(Handle, i, out int o, p, nrm) != 0 && o == other.Handle)
            {
                point = new Float3(p[0], p[1], p[2]); normal = new Float3(nrm[0], nrm[1], nrm[2]);
                return true;
            }
        return false;
    }

    /// <summary>What the areas and the world would give this body now: gravity in cm/s^2 before its gravity factor (Godot total_gravity).</summary>
    public Float3 AreaGravity
    {
        get { var g = new float[3]; return Native.aver_phys_body_area_gravity(Handle, g) != 0 ? new Float3(g[0], g[1], g[2]) : Float3.Zero; }
    }

    /// <summary>The linear and angular damping the areas and the body's own would give it now.</summary>
    public (float Linear, float Angular) AreaDamping
        => Native.aver_phys_body_area_damping(Handle, out float l, out float a) != 0 ? (l, a) : (0f, 0f);
}

/// <summary>A sensor body registered as a Godot-style area. A handle (the sensor's own); 0 is invalid.</summary>
public readonly struct Area : System.IEquatable<Area>
{
    public int Handle { get; }
    public Area(int handle) { Handle = handle; }
    public Body Body => new(Handle);
    public bool IsValid => Native.aver_phys_area_exists(Handle) != 0;

    /// <summary>Registers a sensor body as an area; invalid if the body is not a sensor.</summary>
    public static Area Create(Body sensor) => Native.aver_phys_area_create(sensor.Handle) != 0 ? new Area(sensor.Handle) : default;
    /// <summary>Back to a plain sensor.</summary>
    public bool Destroy() => Native.aver_phys_area_destroy(Handle) != 0;
    public static int Count => Native.aver_phys_area_count();

    /// <summary>Monitoring: the area reports who is inside. Monitorable: other areas can detect it. Both default to true.</summary>
    public bool SetFlags(bool monitoring, bool monitorable) => Native.aver_phys_area_set_flags(Handle, monitoring ? 1 : 0, monitorable ? 1 : 0) != 0;
    public (bool Monitoring, bool Monitorable) Flags
        => Native.aver_phys_area_flags(Handle, out int m, out int b) != 0 ? (m != 0, b != 0) : (false, false);

    /// <summary>The area's layer bits (as a target) and the mask it scans with.</summary>
    public bool SetCollision(uint layerBits, uint maskBits) => Native.aver_phys_area_set_collision(Handle, layerBits, maskBits) != 0;
    public (uint Layer, uint Mask) Collision
        => Native.aver_phys_area_collision(Handle, out uint l, out uint m) != 0 ? (l, m) : (0u, 0u);

    public bool SetPriority(int priority) => Native.aver_phys_area_set_priority(Handle, priority) != 0;

    /// <summary>Gravity override: <paramref name="magnitudeCmS2"/> along <paramref name="direction"/> (world space, normalised natively).</summary>
    public bool SetGravity(AreaOverrideMode mode, float magnitudeCmS2, Float3 direction)
        => Native.aver_phys_area_set_gravity(Handle, (int)mode, magnitudeCmS2, direction.X, direction.Y, direction.Z) != 0;
    /// <summary>Point gravity toward <paramref name="centreLocal"/> (area local, cm); with a unit distance the strength is inverse-square.</summary>
    public bool SetGravityPoint(bool on, Float3 centreLocal, float unitDistanceCm)
        => Native.aver_phys_area_set_gravity_point(Handle, on ? 1 : 0, centreLocal.X, centreLocal.Y, centreLocal.Z, unitDistanceCm) != 0;
    public bool SetDamping(AreaOverrideMode linearMode, float linear, AreaOverrideMode angularMode, float angular)
        => Native.aver_phys_area_set_damping(Handle, (int)linearMode, linear, (int)angularMode, angular) != 0;
    /// <summary>A wind-like force (kg*cm/s^2) on dynamic bodies inside, weakening beyond the unit distance by the attenuation exponent.</summary>
    public bool SetWind(float forceMagnitude, Float3 direction, float attenuation = 0f, float unitDistanceCm = 0f)
        => Native.aver_phys_area_set_wind(Handle, forceMagnitude, direction.X, direction.Y, direction.Z, attenuation, unitDistanceCm) != 0;

    /// <summary>Handles of the bodies and areas inside as of the last fixed step (ascending); empty while monitoring is off.</summary>
    public IReadOnlyList<(int Handle, bool IsArea)> Overlaps
    {
        get
        {
            int n = Native.aver_phys_area_overlap_count(Handle);
            var list = new List<(int, bool)>(n);
            for (int i = 0; i < n; i++)
                if (Native.aver_phys_area_overlap_get(Handle, i, out int o, out int isArea) != 0) list.Add((o, isArea != 0));
            return list;
        }
    }

    public bool Equals(Area other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is Area a && Equals(a);
    public override int GetHashCode() => Handle;
    public static bool operator ==(Area a, Area b) => a.Handle == b.Handle;
    public static bool operator !=(Area a, Area b) => a.Handle != b.Handle;
}

public static partial class Physics
{
    /// <summary>Contact-monitor BEGIN and END records of the last Step call (one per monitored side).</summary>
    public static IReadOnlyList<BodyContactEvent> BodyContactEvents
    {
        get
        {
            int n = Native.aver_phys_contact_event_count();
            var list = new List<BodyContactEvent>(n);
            var p = new float[3]; var nrm = new float[3];
            for (int i = 0; i < n; i++)
                if (Native.aver_phys_contact_event_get(i, out int b, out int o, out int began, p, nrm) != 0)
                    list.Add(new BodyContactEvent(b, o, began != 0, new Float3(p[0], p[1], p[2]), new Float3(nrm[0], nrm[1], nrm[2])));
            return list;
        }
    }

    /// <summary>Area enter and exit records of the last Step call.</summary>
    public static IReadOnlyList<AreaEvent> AreaEvents
    {
        get
        {
            int n = Native.aver_phys_area_event_count();
            var list = new List<AreaEvent>(n);
            for (int i = 0; i < n; i++)
                if (Native.aver_phys_area_event_get(i, out int a, out int o, out int entered, out int isArea) != 0)
                    list.Add(new AreaEvent(a, o, entered != 0, isArea != 0));
            return list;
        }
    }
}
