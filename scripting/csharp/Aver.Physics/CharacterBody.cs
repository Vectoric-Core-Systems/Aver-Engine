// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// The walking capsule: swept and resolved rather than simulated as a rigid body. Named CharacterBody,
// not Character -- Aver.Framework already has a Character type and the two would be easy to confuse
// across the assembly boundary.
//
// Drawn from the SAME handle counter as Body (physics_abi.h's "Entity association" section), so a
// CharacterBody's handle is also a valid Body handle for anything that does not care which family it
// is -- SetEntity, a raycast hit, Joint.WorldBody. See AsBody.

using System;

namespace Aver.Physics;

/// <summary>A capsule that walks. A handle, not an object: 0 is invalid.</summary>
public readonly struct CharacterBody : IEquatable<CharacterBody>
{
    public int Handle { get; }
    public CharacterBody(int handle) { Handle = handle; }

    public static CharacterBody None => new(0);
    public bool IsValid => Handle != 0;

    /// <summary>The same handle, seen as a <see cref="Body"/> -- valid because characters and bodies
    /// share one handle counter. What a raycast hit or a joint's world-body slot actually wants.</summary>
    public Body AsBody => new(Handle);

    /// <summary>Creates a character capsule. <paramref name="height"/> is the TOTAL capsule height,
    /// both hemispherical caps included.</summary>
    public static CharacterBody Create(float radius, float height, Float3 position) =>
        new(Native.aver_phys_character_create(radius, height, position.X, position.Y, position.Z));

    /// <summary>Destroys the character. The handle is dead afterwards.</summary>
    public bool Destroy() => Native.aver_phys_character_destroy(Handle) != 0;

    // ---- Placement and velocity -----------------------------------------------------------------------

    /// <summary>The velocity the character WANTS, cm/s, relative to what it stands on: the physics step
    /// adds the ground's own motion (a moving deck, a lift) on top while it stands there, so zero means
    /// "stay put on the train". The simulation manages the vertical component unless this sets it.</summary>
    public Float3 Velocity
    {
        get { var v = new float[3]; return Native.aver_phys_character_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
    }
    public bool SetVelocity(Float3 v) => Native.aver_phys_character_set_velocity(Handle, v.X, v.Y, v.Z) != 0;

    public Float3 Position
    {
        get { var v = new float[3]; return Native.aver_phys_character_position(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
    }
    /// <summary>Teleports the character.</summary>
    public bool SetPosition(Float3 p) => Native.aver_phys_character_set_position(Handle, p.X, p.Y, p.Z) != 0;

    /// <summary>True while standing on ground steep enough to hold. For the four-way answer (which
    /// tells "sliding on a too-steep slope" apart from "airborne"), see <see cref="GroundState"/>.</summary>
    public bool Grounded => Native.aver_phys_character_grounded(Handle) != 0;

    /// <summary>Stamps this character with the scene entity that owns it, so a raycast against it
    /// reports that entity. Equivalent to <c>AsBody.SetEntity</c>.</summary>
    public bool SetEntity(int entity) => Native.aver_phys_set_entity(Handle, entity) != 0;

    // ---- Max slope angle -------------------------------------------------------------------------------

    /// <summary>The maximum slope angle the character can still walk up, radians. Steeper ground reports
    /// as <see cref="Aver.Physics.GroundState.OnSteepGround"/> rather than OnGround.</summary>
    public float? MaxSlopeAngle
    {
        get { var v = new float[1]; return Native.aver_phys_character_max_slope_angle(Handle, v) != 0 ? v[0] : null; }
    }
    public bool SetMaxSlopeAngle(float radians) => Native.aver_phys_character_set_max_slope_angle(Handle, radians) != 0;

    // ---- Stair stepping ---------------------------------------------------------------------------------
    // A per-update parameter to Jolt's ExtendedUpdate, not a property CharacterVirtual itself stores --
    // recorded here for whatever drives the character's own update loop to consult.

    /// <summary>(stepUpCm, stepDownCm): the tallest stair a WalkStairs pass may climb, and how far a
    /// StickToFloor pass may pull the character back onto ground it lost contact with. Un-set, a
    /// character reports Jolt's own defaults (40cm up, 50cm down), not zero.</summary>
    public (float StepUpCm, float StepDownCm) StairStepping
    {
        get
        {
            var up = new float[1]; var down = new float[1];
            return Native.aver_phys_character_stair_stepping(Handle, up, down) != 0 ? (up[0], down[0]) : (0f, 0f);
        }
    }
    public bool SetStairStepping(float stepUpCm, float stepDownCm) => Native.aver_phys_character_set_stair_stepping(Handle, stepUpCm, stepDownCm) != 0;

    // ---- Ground state and what the character stands on -------------------------------------------------
    // Ground/position/velocity below are only meaningful while GroundState is OnGround or
    // OnSteepGround; Jolt still answers something for NotSupported/InAir (typically whatever it last
    // touched), so check GroundState first rather than trusting these alone.

    /// <summary>Jolt's four-way answer for what the character is standing on, or -1 for a dead handle
    /// (0 is OnGround, a real answer) -- cast to <see cref="Aver.Physics.GroundState"/> when non-negative.</summary>
    public int GroundStateRaw => Native.aver_phys_character_ground_state(Handle);
    /// <summary>As <see cref="GroundStateRaw"/>, or null for a dead handle.</summary>
    public GroundState? GroundState
    {
        get { int s = GroundStateRaw; return s >= 0 ? (GroundState)s : null; }
    }

    /// <summary>The ground's contact normal, unit length.</summary>
    public Float3 GroundNormal
    {
        get { var v = new float[3]; return Native.aver_phys_character_ground_normal(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
    }
    /// <summary>The ground's contact position, engine centimetres.</summary>
    public Float3 GroundPosition
    {
        get { var v = new float[3]; return Native.aver_phys_character_ground_position(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
    }
    /// <summary>The body (or character) being stood on, resolved the same way a raycast hit is --
    /// <see cref="Body.None"/> for standing on something this module gave no handle to (a landscape
    /// heightfield) as well as for standing on nothing at all. Check <see cref="GroundState"/> to tell
    /// those two apart.</summary>
    public Body GroundBody => new(Native.aver_phys_character_ground_body(Handle));
    /// <summary>The ground's own velocity, cm/s, world space -- THIS IS WHAT A MOVING PLATFORM IS. The
    /// physics step already adds it while the character stands there, so the character rides the
    /// platform by itself: do NOT add it to the velocity you set as well, or it moves at twice the
    /// platform's speed. For reading -- a world-space speed, whether a lift is moving.</summary>
    public Float3 GroundVelocity
    {
        get { var v = new float[3]; return Native.aver_phys_character_ground_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
    }
    /// <summary>The motion the character carries from its ground, cm/s, world space: the ground's while it
    /// stands on moving ground, and the horizontal part of it kept in the air after leaving, so a jump on a
    /// moving train lands on the train. <see cref="Velocity"/> + this is the world-space velocity. Set it to
    /// zero for a dead stop in the air, which <see cref="SetVelocity"/> alone cannot do; on moving ground the
    /// next step measures the ground again.</summary>
    public Float3 InheritedVelocity
    {
        get { var v = new float[3]; return Native.aver_phys_character_inherited_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
        set { Native.aver_phys_character_set_inherited_velocity(Handle, value.X, value.Y, value.Z); }
    }

    // ---- Shape (crouching) ------------------------------------------------------------------------------

    /// <summary>Swaps the capsule for a new radius/height -- crouching is a smaller capsule, standing
    /// back up is the same call with the original numbers. <paramref name="maxPenetrationCm"/> is how
    /// much the NEW shape may overlap the world and still be accepted.
    ///
    /// THIS CAN FAIL, AND THAT IS THE POINT: growing the capsule under a low ceiling would embed it in
    /// solid geometry, so false here means "you cannot stand up here" -- the character keeps its old
    /// shape -- not merely an error to log.</summary>
    public bool SetShape(float radius, float height, float maxPenetrationCm) => Native.aver_phys_character_set_shape(Handle, radius, height, maxPenetrationCm) != 0;

    // ---- Mass and push strength -------------------------------------------------------------------------
    // A CharacterVirtual is not a rigid body -- nothing ever applies a force TO it -- but these decide
    // how hard it can push and how much of the world's weight it can be asked to carry when it contacts
    // a dynamic body. Neither affects how the character itself moves.

    /// <summary>The character's mass in kilograms, used to weigh down whatever it stands on. Jolt's own
    /// default is 70.</summary>
    public float Mass
    {
        get { var v = new float[1]; return Native.aver_phys_character_mass(Handle, v) != 0 ? v[0] : 0f; }
    }
    public bool SetMass(float massKg) => Native.aver_phys_character_set_mass(Handle, massKg) != 0;

    /// <summary>The maximum force the character can push other bodies with, kg*cm/s^2. A motor character
    /// shoving a crate stalls once the crate needs more than this to move; Jolt's own default
    /// (~100 Newtons) lets a character shove almost anything.</summary>
    public float MaxStrength
    {
        get { var v = new float[1]; return Native.aver_phys_character_max_strength(Handle, v) != 0 ? v[0] : 0f; }
    }
    public bool SetMaxStrength(float maxStrengthKgCmS2) => Native.aver_phys_character_set_max_strength(Handle, maxStrengthKgCmS2) != 0;

    public bool Equals(CharacterBody other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is CharacterBody c && Equals(c);
    public override int GetHashCode() => Handle;
    public static bool operator ==(CharacterBody a, CharacterBody b) => a.Equals(b);
    public static bool operator !=(CharacterBody a, CharacterBody b) => !a.Equals(b);
    public override string ToString() => IsValid ? $"CharacterBody#{Handle}" : "CharacterBody.None";
}
