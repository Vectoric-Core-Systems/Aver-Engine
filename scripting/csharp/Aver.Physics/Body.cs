// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// A rigid body: placement, the forces/impulses that push it, the material it is made of, and
// sleeping. See physics_abi.h's own "Body dynamics" section comment for the units every force,
// impulse and torque is expressed in, and why angular quantities are axial vectors rather than plain
// directions.
//
// A SOFT BODY IS ALSO A BODY (physics_abi.h), drawn from the same handle counter -- so the soft-body
// accessors live here too rather than on a parallel type, and calling them on a rigid body simply
// returns the ABI's own "not a soft body" answer (0 vertices, a failed apply_impulse) rather than
// throwing.

namespace Aver.Physics;

/// <summary>A body or character in the simulation. A handle, not an object: 0 is invalid, and the two
/// families share one counter (physics_abi.h's "Entity association" section), so a handle that came
/// from <see cref="CharacterBody"/> is also a valid <see cref="Body"/> for anything that does not care
/// which it is -- <see cref="SetEntity"/>, a raycast hit, a joint's world-body constant.</summary>
public readonly struct Body : System.IEquatable<Body>
{
    /// <summary>The raw ABI handle. 0 is invalid.</summary>
    public int Handle { get; }
    public Body(int handle) { Handle = handle; }

    public static Body None => new(0);
    public bool IsValid => Handle != 0;

    // ---- Placement --------------------------------------------------------------------------------

    /// <summary>Centre of mass position, centimetres. Zero for a dead handle.</summary>
    public Float3 Position
    {
        get
        {
            var v = new float[3];
            return Native.aver_phys_body_position(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero;
        }
    }

    /// <summary>Orientation. Identity for a dead handle.</summary>
    public Quaternion Rotation
    {
        get
        {
            var q = new float[4];
            return Native.aver_phys_body_rotation(Handle, q) != 0 ? new Quaternion(q[0], q[1], q[2], q[3]) : Quaternion.Identity;
        }
    }

    /// <summary>Linear velocity, cm/s. Zero for a dead handle.</summary>
    public Float3 Velocity
    {
        get
        {
            var v = new float[3];
            return Native.aver_phys_body_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero;
        }
    }

    /// <summary>Angular velocity, radians per second about each engine axis. Zero for a dead handle.</summary>
    public Float3 AngularVelocity
    {
        get
        {
            var v = new float[3];
            return Native.aver_phys_body_angular_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero;
        }
    }

    /// <summary>Teleports the body, ignoring collision on the way, and wakes it.</summary>
    public bool SetPosition(Float3 p) => Native.aver_phys_body_set_position(Handle, p.X, p.Y, p.Z) != 0;
    public bool SetPosition(float x, float y, float z) => Native.aver_phys_body_set_position(Handle, x, y, z) != 0;

    /// <summary>Sets orientation as a quaternion. Wakes the body.</summary>
    public bool SetRotation(Quaternion q) => Native.aver_phys_body_set_rotation(Handle, q.X, q.Y, q.Z, q.W) != 0;
    public bool SetRotation(float x, float y, float z, float w) => Native.aver_phys_body_set_rotation(Handle, x, y, z, w) != 0;

    /// <summary>Drives a KINEMATIC body to <paramref name="position"/> and <paramref name="rotation"/>
    /// over the next <paramref name="dt"/> seconds. The velocity that gets it there is derived, so
    /// whatever rests on the body is carried along, which <see cref="SetPosition(Float3)"/> does not do.
    /// False for a dead handle, a body that is not kinematic, a non-positive <paramref name="dt"/> or a
    /// non-finite pose. A pose that JUMPED (a reset, a wrapped route) should be set with
    /// <see cref="SetPosition(Float3)"/> and <see cref="SetRotation(Quaternion)"/> instead: driving it
    /// there would fling a passenger. THE DERIVED VELOCITY STAYS ON THE BODY and keeps moving it (and
    /// whoever stands on it) after the last call: to stop a driven body, drive it to the pose it already
    /// has, or zero its linear and angular velocity -- and zero them after such a teleport too.</summary>
    public bool MoveKinematic(Float3 position, Quaternion rotation, float dt) =>
        Native.aver_phys_body_move_kinematic(Handle, position.X, position.Y, position.Z,
                                             rotation.X, rotation.Y, rotation.Z, rotation.W, dt) != 0;

    /// <summary>Sets linear velocity, cm/s. Wakes the body.</summary>
    public bool SetVelocity(Float3 v) => Native.aver_phys_body_set_velocity(Handle, v.X, v.Y, v.Z) != 0;
    public bool SetVelocity(float x, float y, float z) => Native.aver_phys_body_set_velocity(Handle, x, y, z) != 0;

    /// <summary>Sets angular velocity, radians/s about each engine axis. Wakes the body.</summary>
    public bool SetAngularVelocity(Float3 w) => Native.aver_phys_body_set_angular_velocity(Handle, w.X, w.Y, w.Z) != 0;
    public bool SetAngularVelocity(float wx, float wy, float wz) => Native.aver_phys_body_set_angular_velocity(Handle, wx, wy, wz) != 0;

    /// <summary>Adds to the current velocity rather than replacing it -- what an accumulating effect
    /// (wind, a chain of knockbacks) wants; use <see cref="SetVelocity(Float3)"/> for "this is how fast
    /// it is going now".</summary>
    public bool AddVelocity(Float3 delta) => Native.aver_phys_body_add_velocity(Handle, delta.X, delta.Y, delta.Z) != 0;

    // ---- Motion type and lifetime -------------------------------------------------------------------

    /// <summary>Static, Kinematic or Dynamic. -1 (not a valid <see cref="MotionType"/>) for a dead
    /// handle -- 0 is Static, a real answer.</summary>
    public int MotionTypeRaw => Native.aver_phys_body_motion_type(Handle);

    /// <summary>Changes motion type, activating the body if it becomes movable.</summary>
    public bool SetMotionType(MotionType type) => Native.aver_phys_body_set_motion_type(Handle, (int)type) != 0;

    /// <summary>Removes the body from the world and destroys it. The handle is dead afterwards.</summary>
    public bool Destroy() => Native.aver_phys_remove_body(Handle) != 0;

    // ---- Forces and impulses --------------------------------------------------------------------------
    // A force lasts one physics step and must be re-applied to push continuously; an impulse changes
    // velocity instantly by impulse/mass and does not accumulate. See physics_abi.h's own section
    // comment for the unit derivations (force is kg*cm/s^2, torque is kg*cm^2/s^2, and so on).

    /// <summary>A force through the centre of mass, kg*cm/s^2, for this step only.</summary>
    public bool AddForce(Float3 f) => Native.aver_phys_body_add_force(Handle, f.X, f.Y, f.Z) != 0;
    /// <summary>The same force, applied at a world-space point -- also produces torque about the
    /// centre of mass.</summary>
    public bool AddForceAt(Float3 f, Float3 atPoint) => Native.aver_phys_body_add_force_at(Handle, f.X, f.Y, f.Z, atPoint.X, atPoint.Y, atPoint.Z) != 0;
    /// <summary>A torque about the centre of mass, kg*cm^2/s^2, for this step only.</summary>
    public bool AddTorque(Float3 t) => Native.aver_phys_body_add_torque(Handle, t.X, t.Y, t.Z) != 0;

    /// <summary>An instantaneous impulse through the centre of mass, kg*cm/s: velocity changes by
    /// impulse/mass. A jump, a bullet hit, an explosion.</summary>
    public bool AddImpulse(Float3 i) => Native.aver_phys_body_add_impulse(Handle, i.X, i.Y, i.Z) != 0;
    /// <summary>The same at a world-space point, so an off-centre hit spins the body as well as
    /// moving it.</summary>
    public bool AddImpulseAt(Float3 i, Float3 atPoint) => Native.aver_phys_body_add_impulse_at(Handle, i.X, i.Y, i.Z, atPoint.X, atPoint.Y, atPoint.Z) != 0;
    /// <summary>An instantaneous angular impulse, kg*cm^2/s.</summary>
    public bool AddAngularImpulse(Float3 a) => Native.aver_phys_body_add_angular_impulse(Handle, a.X, a.Y, a.Z) != 0;

    // ---- Material and mass ------------------------------------------------------------------------------

    /// <summary>0 is frictionless ice, 1 is roughly rubber; Jolt's own default is 0.2. Two touching
    /// bodies combine theirs (geometric mean).</summary>
    public float Friction
    {
        get { var v = new float[1]; return Native.aver_phys_body_friction(Handle, v) != 0 ? v[0] : 0f; }
    }
    public bool SetFriction(float friction) => Native.aver_phys_body_set_friction(Handle, friction) != 0;

    /// <summary>0 keeps none of the approach speed, 1 keeps all of it (solver damping means a real 1.0
    /// still settles). Jolt's own default is 0.</summary>
    public float Restitution
    {
        get { var v = new float[1]; return Native.aver_phys_body_restitution(Handle, v) != 0 ? v[0] : 0f; }
    }
    public bool SetRestitution(float restitution) => Native.aver_phys_body_set_restitution(Handle, restitution) != 0;

    /// <summary>Scales world gravity for this one body: 1 is normal, 0 floats, negative falls up.</summary>
    public float GravityFactor
    {
        get { var v = new float[1]; return Native.aver_phys_body_gravity_factor(Handle, v) != 0 ? v[0] : 1f; }
    }
    public bool SetGravityFactor(float factor) => Native.aver_phys_body_set_gravity_factor(Handle, factor) != 0;

    /// <summary>Velocity bleed per second (linear, angular). 0 is a vacuum; a small positive pair is
    /// how a body stops instead of sliding forever on a frictionless floor.</summary>
    public (float Linear, float Angular) Damping
    {
        get
        {
            var lin = new float[1]; var ang = new float[1];
            return Native.aver_phys_body_damping(Handle, lin, ang) != 0 ? (lin[0], ang[0]) : (0f, 0f);
        }
    }
    public bool SetDamping(float linear, float angular) => Native.aver_phys_body_set_damping(Handle, linear, angular) != 0;

    /// <summary>Mass in kilograms. DYNAMIC BODIES ONLY -- a static or kinematic body has infinite mass
    /// by definition and reports 0. The setter RESCALES the inertia the shape already implies, so the
    /// body keeps rotating like its own shape at its new weight.</summary>
    public float Mass
    {
        get { var v = new float[1]; return Native.aver_phys_body_mass(Handle, v) != 0 ? v[0] : 0f; }
    }
    public bool SetMass(float massKg) => Native.aver_phys_body_set_mass(Handle, massKg) != 0;

    // ---- Sleeping ---------------------------------------------------------------------------------------

    /// <summary>True while awake and being simulated; false while asleep or for a dead handle.</summary>
    public bool IsActive => Native.aver_phys_body_is_active(Handle) != 0;
    /// <summary>Wakes a sleeping body. Every force/impulse call above already does this; use it for a
    /// body nothing is about to push (a neighbour that was just removed).</summary>
    public bool Activate() => Native.aver_phys_body_activate(Handle) != 0;
    public bool Deactivate() => Native.aver_phys_body_deactivate(Handle) != 0;

    // ---- Layer --------------------------------------------------------------------------------------

    /// <summary>Which collision layer the body is on (0..15), or -1 for a dead handle -- 0 is a real
    /// layer. Changing it never changes whether the body is static or dynamic.</summary>
    public int Layer => Native.aver_phys_body_layer(Handle);
    public bool SetLayer(int layer) => Native.aver_phys_body_set_layer(Handle, layer) != 0;

    // ---- Entity association ---------------------------------------------------------------------------

    /// <summary>Stamps this body (or character) with the scene entity that owns it, so a later raycast
    /// against it reports that entity. Backed by Jolt's own per-body user-data field.</summary>
    public bool SetEntity(int entity) => Native.aver_phys_set_entity(Handle, entity) != 0;

    // ---- Per-body water override --------------------------------------------------------------------
    // Takes precedence over Physics.SetWaterPlane for this body -- a puddle or tank at a different
    // height from the world's main surface.

    public bool SetWaterVolume(Float3 surfacePos, Float3 surfaceNormal, float buoyancy, float linearDrag, float angularDrag, Float3 fluidVelocity) =>
        Native.aver_phys_set_water_volume(Handle, surfacePos.ToArray(), surfaceNormal.ToArray(), buoyancy, linearDrag, angularDrag, fluidVelocity.ToArray()) != 0;

    /// <summary>Removes this body's override, returning it to the global plane if one is set. False
    /// for a dead handle or a body that had no override.</summary>
    public bool ClearWaterVolume() => Native.aver_phys_clear_water_volume(Handle) != 0;

    // ---- Soft body ----------------------------------------------------------------------------------
    // A soft body IS a body (see the file comment): calling these on a rigid body handle is not an
    // error, it just reports "not a soft body" the way the ABI itself does -- 0 vertices, a failed
    // ApplyImpulse.

    /// <summary>How many particles this soft body has, or 0 for any other handle.</summary>
    public int SoftBodyVertexCount => Native.aver_phys_softbody_vertex_count(Handle);

    /// <summary>Reads the deformed particle positions, world-space engine centimetres. Returns how
    /// many were actually written, which may be less than <paramref name="maxVertices"/>.</summary>
    public int SoftBodyVertices(float[] outXyz, int maxVertices) => Native.aver_phys_softbody_vertices(Handle, outXyz, maxVertices);

    /// <summary>Drives a skinned soft body from an animated pose. Call BEFORE <see cref="Physics.Step"/>
    /// each frame. <paramref name="hardSkin"/> true snaps every particle onto its skinned position
    /// instead of constraining toward it -- pass true on the first frame and after a teleport.
    /// <paramref name="jointMatrices"/> is <paramref name="jointCount"/> row-major 4x4 matrices, exactly
    /// what aver::anim::poseToSkinning produces. False for a handle that is not a skinned soft body.</summary>
    public bool SoftBodySkin(float[] jointMatrices, int jointCount, bool hardSkin) =>
        Native.aver_phys_softbody_skin(Handle, jointMatrices, jointCount, hardSkin ? 1 : 0) != 0;

    /// <summary>Blends the velocity of every particle within <paramref name="radiusCm"/> of
    /// <paramref name="centre"/> a <paramref name="strength"/> fraction of the way toward
    /// <paramref name="velocity"/> -- a BLEND, not an added impulse, so calling this every step while
    /// something stays near the volume converges instead of running away. Modifies velocity only, never
    /// a particle's position. Returns how many particles were nudged.</summary>
    public int SoftBodyApplyImpulse(Float3 centre, float radiusCm, Float3 velocity, float strength) =>
        Native.aver_phys_softbody_apply_impulse(Handle, centre.ToArray(), radiusCm, velocity.ToArray(), strength);

    // ---- Equality -----------------------------------------------------------------------------------

    public bool Equals(Body other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is Body b && Equals(b);
    public override int GetHashCode() => Handle;
    public static bool operator ==(Body a, Body b) => a.Equals(b);
    public static bool operator !=(Body a, Body b) => !a.Equals(b);
    public override string ToString() => IsValid ? $"Body#{Handle}" : "Body.None";
}
