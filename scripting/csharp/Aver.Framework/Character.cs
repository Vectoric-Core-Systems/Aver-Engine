// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Walking character pawn: capsule physics, mouse look, and a head node the camera rides on.

using System;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>Which view the camera uses while a character is possessed.</summary>
public enum CameraView
{
    /// <summary>Camera at the character's eyes, looking where it faces.</summary>
    FirstPerson,
    /// <summary>Camera behind and above, looking at the character.</summary>
    ThirdPerson,
}

/// <summary>A pawn for a walking character, first- or third-person, driven by WASD and the mouse.</summary>
/// <remarks>The entity's origin is the character's FEET, not the capsule centre.
///
/// CONCRETE, DELIBERATELY -- this is the framework's own spawnable "Character" registry row (see
/// DeclareBaseClasses in HostBridge.cs), not just a C# base a project subclasses. Everything else the
/// framework hands out as a lineage root (Actor/Pawn/PlayerController/GameMode/GameInstance) is an
/// ABSTRACT anchor with no behaviour of its own -- a project or a graph must always supply a concrete
/// child. A character is different: the capsule, the view node, the pitch clamp and Drive/DriveFromGraph
/// ARE the useful behaviour, none of it project-specific, and there is no reason a `CLASS AN_Player
/// Character` graph -- or a bare level placement of "Character" itself -- should have to wait for a
/// project to write a C# subclass just to re-declare machinery this file already owns. Unreal's
/// ACharacter is concrete for the identical reason; a second, near-empty concrete subclass purely to
/// dodge the word "abstract" would be ceremony, not a different design.
///
/// A graph parented (directly or through other graph classes) to "Character" gets a REAL instance of
/// exactly this class constructed alongside its GraphHost -- see DispBind's own comment in HostBridge.cs
/// for the two-tables-one-entity mechanism that makes that true, and CharacterMoveForGraph, below, for
/// what finds it.
///
/// [AverClass]'s Parent is stated EXPLICITLY as "Pawn" rather than left at its own "Actor" default:
/// the default would route through HostBridge.BaseRegistryName, which (deliberately, for every OTHER
/// AverCharacter subclass) resolves an AverCharacter-assignable type to "Character" itself -- exactly
/// wrong for THIS declaration, which IS "Character" and must not parent to itself.</remarks>
[AverClass("Character", Parent = "Pawn")]
public class AverCharacter : AverPawn
{
    /// <summary>Ground move speed, centimetres per second.</summary>
    public float MoveSpeed = 350f;

    /// <summary>Mouse-look sensitivity, degrees of yaw per pixel of mouse movement.</summary>
    public float TurnSpeed = 0.2f;

    /// <summary>Which view the camera should use while this character is possessed.</summary>
    public CameraView CameraViewMode = CameraView.ThirdPerson;

    /// <summary>First-person eye height above the character's origin (its feet), centimetres.</summary>
    public float EyeHeight = 160f;

    /// <summary>Third-person camera distance behind the character, centimetres.</summary>
    public float BoomLength = 450f;

    /// <summary>Total capsule height including both caps, centimetres.</summary>
    public float Height = 180f;

    /// <summary>Capsule radius, centimetres. Must be less than half of <see cref="Height"/>.</summary>
    public float Radius = 34f;

    /// <summary>Upward speed a jump starts with, cm/s.</summary>
    public float JumpSpeed = 465f;

    /// <summary>How far the view may look down / up, degrees.</summary>
    public float PitchMin = -85f, PitchMax = 85f;

    private float _yaw;         // degrees
    private bool  _yawSeeded;   // has _yaw been taken from the spawn transform yet -- see Drive
    private float _pitch;       // degrees
    private int   _capsule;     // physics character handle; 0 when none
    private Entity _view;

    /// <summary>The character's head: a child entity at eye height carrying the view pitch.</summary>
    public Entity View => _view;

    /// <summary>The character's facing yaw, degrees, in (-180, 180].</summary>
    public float Yaw => _yaw;

    /// <summary>The view pitch, degrees, clamped to <see cref="PitchMin"/>..<see cref="PitchMax"/>.</summary>
    public float Pitch => _pitch;

    /// <summary>Where the eyes are: the character's feet plus <see cref="EyeHeight"/>.</summary>
    public Vec3 EyePosition => Self.LocalPosition + Vec3.Up * EyeHeight;

    /// <summary>The unit direction the character is looking: yaw and pitch.</summary>
    public Vec3 LookDirection
    {
        get
        {
            float y = _yaw * (MathF.PI / 180f), p = _pitch * (MathF.PI / 180f);
            float cp = MathF.Cos(p);
            return new Vec3(MathF.Cos(y) * cp, MathF.Sin(y) * cp, MathF.Sin(p));
        }
    }

    /// <summary>The ground-plane direction the character walks when moving forward.</summary>
    public Vec3 WalkForward
    {
        get { float y = _yaw * (MathF.PI / 180f); return new Vec3(MathF.Cos(y), MathF.Sin(y), 0f); }
    }

    /// <summary>The ground-plane direction to the character's right.</summary>
    public Vec3 WalkRight
    {
        get { float y = _yaw * (MathF.PI / 180f); return new Vec3(-MathF.Sin(y), MathF.Cos(y), 0f); }
    }

    /// <summary>True when this character is backed by the physics world rather than translating directly.</summary>
    public bool IsSimulated => _capsule != 0;

    /// <summary>True while standing on ground shallow enough to hold. False when not simulated.</summary>
    public bool IsGrounded => _capsule != 0 && Phys.aver_phys_character_grounded(_capsule) != 0;

    /// <summary>The character's own velocity, cm/s, relative to what it stands on. Riding a moving deck
    /// or a lift, the physics step adds the ground's motion on top and it is not in this value, so a
    /// passenger standing still on a train reads (and keeps) zero.</summary>
    public Vec3 Velocity
    {
        get
        {
            if (_capsule == 0) return Vec3.Zero;
            float[] v = new float[3];
            return Phys.aver_phys_character_velocity(_capsule, v) != 0 ? new Vec3(v[0], v[1], v[2]) : Vec3.Zero;
        }
        /// <summary>Replaces the character's momentum outright -- a launch pad, a dash, a dead stop.
        ///
        /// A SETTER RATHER THAN AN AddForce: this is a CharacterVirtual, not a rigid body, and it is
        /// integrated from a velocity its owner supplies each step. Silently does nothing when the
        /// character is not simulated, exactly as the getter returns zero there, so neither a script
        /// nor a graph has to test IsSimulated before using it.
        ///
        /// OUTRIGHT INCLUDES WHAT THE GROUND LENT IT: a character that jumped off a moving train still
        /// carries the train's horizontal motion (aver_phys_character_inherited_velocity), which its own
        /// velocity does not hold, so that is cleared too -- otherwise a dead stop in the air would keep
        /// drifting at the train's speed. Standing on moving ground, the next step lends it again.</summary>
        set
        {
            if (_capsule == 0) return;
            Phys.aver_phys_character_set_velocity(_capsule, value.X, value.Y, value.Z);
            Phys.aver_phys_character_set_inherited_velocity(_capsule, 0f, 0f, 0f);
        }
    }

    /// <summary>Jumps if grounded. Returns false when airborne.</summary>
    public bool Jump()
    {
        if (!IsGrounded) return false;
        Vec3 v = Velocity;
        return Phys.aver_phys_character_set_velocity(_capsule, v.X, v.Y, JumpSpeed) != 0;
    }

    /// <summary>Runs one frame of character control from the current device state.</summary>
    protected void DriveWithInput(float dt) =>
        Drive(dt, Input.MoveAxis, Input.MouseDeltaX * TurnSpeed, -Input.MouseDeltaY * TurnSpeed);

    /// <summary>Runs one frame of character control from graph-supplied move and look values --
    /// Aver.Graph's own entry point onto <see cref="Drive"/>, reached through
    /// GraphInterop.CharacterMoveForGraph. <c>internal</c>, not <c>protected</c>, and that is
    /// deliberate: GraphInterop lives in this SAME assembly (so <c>internal</c> is reachable) but is
    /// a DIFFERENT class from AverCharacter, so even same-assembly code cannot reach a
    /// <c>protected</c> member through a plain reference the way a subclass could -- a cast
    /// (<c>((AverCharacter)actor).Drive(...)</c>) does not compile across that boundary either. This
    /// is a same-assembly forwarding call for Aver.Framework's OWN graph-interop surface, the same
    /// reason <see cref="AverActor.OnUnbound"/> is <c>internal</c> rather than <c>public</c> --  not
    /// a new public API a project's Scripts.dll could call to bypass its own subclass's OnTick.
    /// <see cref="Drive"/> itself is untouched: the pitch clamp and the capsule stay in the one
    /// tested place, exactly as the task requires.</summary>
    internal void DriveFromGraph(float dt, Vec3 moveAxis, float yawDeltaDeg, float pitchDeltaDeg) =>
        Drive(dt, moveAxis, yawDeltaDeg, pitchDeltaDeg);

    /// <summary>Runs one frame of character control from supplied move and look values.</summary>
    /// <param name="moveAxis">X = forward intent, Y = right intent, each -1..1.</param>
    protected void Drive(float dt, Vec3 moveAxis, float yawDeltaDeg, float pitchDeltaDeg)
    {
        // A CHARACTER KEEPS THE FACING IT WAS PLACED WITH, seeded on the first drive.
        //
        // _yaw starts at 0 and was never seeded from anything, so ApplyLookRotation's first call
        // wrote yaw 0 straight over whatever rotation the spawn gave the entity. A character placed
        // facing down a corridor, or spawned at a Player Start with a yaw, snapped to +X on its
        // first frame with no way to say otherwise -- SetYaw is protected, so only the character's
        // own subclass could correct it and native code placing a pawn had no route at all.
        //
        // HERE AND NOT IN EnsureCapsule, WHICH IS WHERE I PUT IT FIRST AND WAS WRONG. The capsule is
        // created during begin_play, BEFORE the editor's Play path gets a chance to move the pawn on
        // to the Player Start -- so seeding there read the pre-placement rotation, got 0, and the
        // Player Start's yaw was still discarded. Measured: a spawn authored yaw 180 produced a view
        // 1.1% different from the same spawn authored yaw 0, when a half turn should change every
        // pixel. Drive runs after startPlay within the same frame, so the first call here is the
        // earliest point at which the placed rotation is definitely the real one.
        //
        // READ BACK RATHER THAN TRACKED. A pawn moved by ANY route -- level placement, a Player
        // Start, a graph, a C# spawn -- ends at the same entity transform, so reading it honours all
        // of them without each having to remember to announce itself.
        //
        // AND NOT ONLY ON THE FIRST DRIVE. A facing given later is taken the same way: the editor's
        // Pawn to Camera (Shift+F while ejected) turns the pawn to the camera, and like every native
        // caller it can only write the entity and its view node -- _yaw and _pitch are private, and
        // ApplyLookRotation below would put the old facing straight back over its. So a rotation this
        // class did not write is adopted instead of overwritten. One it did write reads back within
        // float noise, far inside the tolerance, so an undisturbed character never re-seeds.
        //
        // aver_fw_set_view STAYS THE FIRST NATIVE CALL: CharacterMoveNodeTests proves a graph reached
        // this method by the EntryPointNotFoundException that call raises in a process with no engine.
        Fw.aver_fw_set_view((int)CameraViewMode, EyeHeight, BoomLength);

        float placedYaw = YawDegreesFromQuat(Self.LocalRotation);
        if (!_yawSeeded || MathF.Abs(DeltaDegrees(placedYaw, _yaw)) > AdoptToleranceDeg)
        {
            _yawSeeded = true;
            _yaw = placedYaw;
        }

        EnsureView();
        // The pitch lives on the view node (ApplyLookRotation), so that is where a given one is read.
        // After EnsureView: on the first drive it has only just written _pitch there itself.
        if (_view.IsAlive)
        {
            float placedPitch = -PitchDegreesFromQuat(_view.LocalRotation);
            if (MathF.Abs(placedPitch - _pitch) > AdoptToleranceDeg)
                _pitch = MathF.Max(PitchMin, MathF.Min(PitchMax, placedPitch));
        }

        // Wrapped, so the read-back above stays exact: an unwrapped yaw spun past ~120,000 degrees loses
        // enough float precision through Rot.ToQuat to miss the tolerance and re-seed on its own.
        _yaw = DeltaDegrees(_yaw + yawDeltaDeg, 0f);
        _pitch = MathF.Max(PitchMin, MathF.Min(PitchMax, _pitch + pitchDeltaDeg));
        ApplyLookRotation();

        EnsureCapsule();

        Vec3 wish = WalkForward * moveAxis.X + WalkRight * moveAxis.Y;
        wish = wish.Normalized * MoveSpeed;

        if (_capsule == 0)
        {
            if (!wish.IsNearlyZero) Self.Translate(wish * dt);
            return;
        }

        SyncFromSimulation();

        Vec3 v = Velocity;
        Phys.aver_phys_character_set_velocity(_capsule, wish.X, wish.Y, v.Z);
    }

    // Yaw about +Z out of a quaternion, in degrees, matching Rot(yaw,0,0).ToQuat()'s own convention
    // so that seeding from a rotation this class itself wrote is exactly a round trip.
    private static float YawDegreesFromQuat(Quat q)
    {
        // Standard atan2 extraction of rotation about Z. A pure yaw quaternion round-trips exactly;
        // a rotation carrying pitch or roll contributes only its yaw component, which is the right
        // answer for a character that has no roll and tracks pitch separately.
        float siny = 2f * (q.W * q.Z + q.X * q.Y);
        float cosy = 1f - 2f * (q.Y * q.Y + q.Z * q.Z);
        return MathF.Atan2(siny, cosy) * (180f / MathF.PI);
    }

    // How far a read-back facing may sit from the one this class holds before it counts as given from
    // outside, degrees. A quaternion this class wrote round-trips to within about 1e-4.
    private const float AdoptToleranceDeg = 0.01f;

    // The rotation about +Y (Right) of a quaternion that turns about +Y alone, in degrees -- the view
    // node's, which ApplyLookRotation writes as Rot(0, -_pitch, 0), so its NEGATION is the pitch.
    private static float PitchDegreesFromQuat(Quat q) =>
        2f * MathF.Atan2(q.Y, q.W) * (180f / MathF.PI);

    // a - b, wrapped into (-180, 180]. Also what keeps _yaw itself in that range (Drive).
    private static float DeltaDegrees(float a, float b)
    {
        float d = (a - b) % 360f;
        if (d > 180f) d -= 360f;
        else if (d <= -180f) d += 360f;
        return d;
    }

    /// <summary>Snaps the facing yaw, degrees.</summary>
    protected void SetYaw(float yawDegrees)
    {
        _yaw = DeltaDegrees(yawDegrees, 0f);   // the same (-180, 180] Drive keeps it in
        ApplyLookRotation();
    }

    /// <summary>Creates the view node if absent and publishes it as the camera transform. Idempotent.</summary>
    public void EnsureView()
    {
        if (_view.IsAlive)
        {
            _view.SetLocalPosition(Vec3.Up * EyeHeight);
            Fw.aver_fw_set_view_entity(_view.Handle);
            return;
        }
        int e = SceneNative.aver_scene_create();
        if (e == 0) return;
        _view = new Entity(e);
        _view.SetName("View");
        _view.SetParent(Self);
        _view.SetLocalPosition(Vec3.Up * EyeHeight);
        ApplyLookRotation();
        Fw.aver_fw_set_view_entity(_view.Handle);
    }

    // Writes yaw to the body and pitch to the head.
    //
    // THE PITCH IS NEGATED, and that is the fix rather than a quirk. Rot.ToQuat builds pitch as
    // FromAxisAngle(Vec3.Right = (0,1,0), pitch), and rotating Forward = (1,0,0) about +Y by a
    // POSITIVE angle sends it to (cos, 0, -sin) -- toward -Z, which in this engine's +Z-up basis is
    // DOWNWARD.
    //
    // Everything else in this class means the opposite by positive pitch. LookDirection returns
    // z = sin(_pitch), so positive is up. DriveWithInput passes -Input.MouseDeltaY so that pushing
    // the mouse forward raises _pitch. PitchMin/PitchMax are written as if positive were up too.
    //
    // So the view entity pitched the wrong way: mouse-look was vertically inverted, and worse,
    // LookDirection -- which is what aiming, traces and any "what am I pointing at" code uses --
    // disagreed with where the camera actually pointed. Two wrongs that did NOT cancel.
    private void ApplyLookRotation()
    {
        Self.SetLocalRotation(new Rot(_yaw, 0f, 0f).ToQuat());
        if (_view.IsAlive) _view.SetLocalRotation(new Rot(0f, -_pitch, 0f).ToQuat());
    }

    /// <summary>Moves the character and its capsule to a feet position, clearing velocity.</summary>
    public void Teleport(Vec3 feetPosition)
    {
        Self.SetLocalPosition(feetPosition);
        EnsureCapsule();
        if (_capsule != 0)
        {
            Vec3 c = feetPosition + Vec3.Up * (Height * 0.5f);
            Phys.aver_phys_character_set_position(_capsule, c.X, c.Y, c.Z);
            Phys.aver_phys_character_set_velocity(_capsule, 0f, 0f, 0f);
        }
    }

    // Releases the capsule when the host unbinds this instance.
    internal override void OnUnbound()
    {
        if (_capsule != 0) { Phys.aver_phys_character_destroy(_capsule); _capsule = 0; }
        base.OnUnbound();
    }

    // Creates the physics capsule on first use.
    private void EnsureCapsule()
    {
        if (_capsule != 0 || !Physics.Ready) return;

        Vec3 feet = Self.LocalPosition;
        Vec3 centre = feet + Vec3.Up * (Height * 0.5f);
        _capsule = Phys.aver_phys_character_create(Radius, Height, centre.X, centre.Y, centre.Z);
        // Without this, a ray fired at this character resolves to entity 0 -- indistinguishable from
        // hitting something ownerless -- and shooting a character can never identify its target.
        if (_capsule != 0) Phys.aver_phys_set_entity(_capsule, Self.Handle);
    }

    // Copies the settled capsule centre back to the entity origin (the feet).
    private void SyncFromSimulation()
    {
        float[] p = new float[3];
        if (Phys.aver_phys_character_position(_capsule, p) == 0) return;
        Self.SetLocalPosition(new Vec3(p[0], p[1], p[2] - Height * 0.5f));
    }
}
