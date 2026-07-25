using System;
using Aver.Scene;   // Vec3, Rot

namespace Aver.Framework;

/// <summary>Which view the camera uses while a character is possessed.</summary>
public enum CameraView
{
    /// <summary>Camera at the character's eyes, looking where it faces.</summary>
    FirstPerson,
    /// <summary>Camera behind and above, looking at the character.</summary>
    ThirdPerson,
}

/// <summary>
/// A pawn built for a walking character, first- or third-person. Over <see cref="AverPawn"/> it adds
/// WASD + mouse control: <see cref="DriveWithInput"/> turns the character with the mouse and walks it
/// in its own facing plane. The editor reads <see cref="CameraViewMode"/>, <see cref="EyeHeight"/> and
/// <see cref="BoomLength"/> off the possessed character to place the play camera.
/// </summary>
/// <remarks>
/// Movement is simulated: the character is a capsule in the physics world that falls under gravity, is
/// pushed out of geometry and walks up slopes it can hold. It is a swept-and-resolved character, not a
/// rigid body — which is what makes it feel controlled rather than thrown.
///
/// <para>The entity's origin is the character's FEET, not the capsule's centre. That is the convention
/// <see cref="EyeHeight"/> already assumed, and it is the one that makes "place it on the ground" mean
/// setting z to the ground height.</para>
///
/// <para>If physics is unavailable — compiled out, or failed to start — the character falls back to the
/// old kinematic translate so a level still runs. <see cref="IsSimulated"/> says which is happening,
/// rather than leaving you to infer it from a character that walks through walls.</para>
/// </remarks>
public abstract class AverCharacter : AverPawn
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

    /// <summary>Total capsule height including both caps, centimetres. Set before the first tick.</summary>
    public float Height = 180f;

    /// <summary>Capsule radius, centimetres. Must be less than half of <see cref="Height"/>.</summary>
    public float Radius = 34f;

    /// <summary>Upward speed a jump starts with, cm/s. Roughly 110cm of height at one g.</summary>
    public float JumpSpeed = 465f;

    /// <summary>How far the view may look down / up, degrees. Stops the view going over the top.</summary>
    public float PitchMin = -85f, PitchMax = 85f;

    private float _yaw;         // facing in degrees, turned by the mouse
    private float _pitch;       // view pitch in degrees, also from the mouse
    private int   _capsule;     // physics character handle; 0 until created, or when unavailable

    /// <summary>The character's facing yaw, degrees. Turned by <see cref="DriveWithInput"/>.</summary>
    public float Yaw => _yaw;

    /// <summary>The view pitch, degrees, clamped to <see cref="PitchMin"/>..<see cref="PitchMax"/>.</summary>
    public float Pitch => _pitch;

    /// <summary>Where the eyes are: the character's origin (its feet) plus <see cref="EyeHeight"/>.</summary>
    public Vec3 EyePosition => Self.LocalPosition + Vec3.Up * EyeHeight;

    /// <summary>
    /// The unit direction the character is LOOKING — yaw and pitch. Distinct from the direction it
    /// walks, which ignores pitch: looking at the sky should not launch you into it.
    /// </summary>
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

    /// <summary>True while standing on ground shallow enough to hold. Always false when not simulated.</summary>
    public bool IsGrounded => _capsule != 0 && Phys.aver_phys_character_grounded(_capsule) != 0;

    /// <summary>
    /// The character's current velocity, cm/s. The vertical component is the simulation's — it is how
    /// you tell a fall from a walk.
    /// </summary>
    public Vec3 Velocity
    {
        get
        {
            if (_capsule == 0) return Vec3.Zero;
            float[] v = new float[3];
            return Phys.aver_phys_character_velocity(_capsule, v) != 0 ? new Vec3(v[0], v[1], v[2]) : Vec3.Zero;
        }
    }

    /// <summary>
    /// Jump, if standing on something. Returns false when airborne, so a caller can play a failure cue
    /// rather than having the jump silently swallowed.
    /// </summary>
    public bool Jump()
    {
        if (!IsGrounded) return false;
        Vec3 v = Velocity;
        return Phys.aver_phys_character_set_velocity(_capsule, v.X, v.Y, JumpSpeed) != 0;
    }

    /// <summary>
    /// Standard character control for one frame: turn with the mouse (yaw about up), then walk with WASD /
    /// arrows in the facing plane at <see cref="MoveSpeed"/>. Call from <c>OnTick</c> while this character is
    /// the possessed pawn.
    /// </summary>
    /// <remarks>
    /// Order matters and is deliberate: the settled result of the last step is read into the entity FIRST,
    /// then this frame's intent is written as a desired velocity. So a script that reads
    /// <c>Self.LocalPosition</c> after calling this sees where the character actually is, not where it was
    /// asked to go — the two differ every time something is in the way, which is the whole point of having
    /// a simulation.
    /// </remarks>
    protected void DriveWithInput(float dt) =>
        Drive(dt, Input.MoveAxis, Input.MouseDeltaX * TurnSpeed, -Input.MouseDeltaY * TurnSpeed);

    /// <summary>
    /// The same frame of character control, but driven by values you supply rather than by polling the
    /// device — so a game using an action-mapping layer (or a replay, or an AI) can drive a character
    /// without the character knowing where the numbers came from.
    /// </summary>
    /// <param name="moveAxis">X = forward intent, Y = right intent, each -1..1.</param>
    /// <param name="yawDeltaDeg">Degrees to turn this frame.</param>
    /// <param name="pitchDeltaDeg">Degrees to pitch this frame; positive looks up.</param>
    protected void Drive(float dt, Vec3 moveAxis, float yawDeltaDeg, float pitchDeltaDeg)
    {
        // Publish the camera this character wants, so the editor's play view can follow it.
        Fw.aver_fw_set_view((int)CameraViewMode, EyeHeight, BoomLength);

        _yaw += yawDeltaDeg;
        _pitch = MathF.Max(PitchMin, MathF.Min(PitchMax, _pitch + pitchDeltaDeg));
        ApplyLookRotation();

        EnsureCapsule();

        // Walk on the YAW basis, never on the entity's forward axis: in first person the transform
        // carries pitch so the camera can aim, and moving along a pitched forward would walk you into
        // the ground or the sky depending on where you happened to be looking.
        Vec3 wish = WalkForward * moveAxis.X + WalkRight * moveAxis.Y;
        // Normalise so a diagonal is not faster than a straight line.
        wish = wish.Normalized * MoveSpeed;

        if (_capsule == 0)
        {
            // No simulation: the old kinematic translate, so a level still runs without physics.
            if (!wish.IsNearlyZero) Self.Translate(wish * dt);
            return;
        }

        SyncFromSimulation();

        // Keep the simulation's vertical velocity: gravity and ground-snapping own it, and overwriting it
        // with zero every frame is what makes a character hover instead of fall.
        Vec3 v = Velocity;
        Phys.aver_phys_character_set_velocity(_capsule, wish.X, wish.Y, v.Z);
    }

    /// <summary>Snap the facing yaw (degrees) — e.g. to spawn a character facing a direction without the
    /// mouse having moved it there.</summary>
    protected void SetYaw(float yawDegrees)
    {
        _yaw = yawDegrees;
        ApplyLookRotation();
    }

    // The transform carries pitch only in FIRST person, because the play camera derives its look
    // direction from the pawn's forward axis -- so that is the only way to aim up or down. In third
    // person the body stays upright instead, since there the camera looks AT the character and a
    // pitched body would just be a character lying over at an angle.
    private void ApplyLookRotation()
    {
        float pitch = CameraViewMode == CameraView.FirstPerson ? _pitch : 0f;
        Self.SetLocalRotation(new Rot(_yaw, pitch, 0f).ToQuat());
    }

    /// <summary>Move the character, simulation included, rather than only its entity transform.</summary>
    /// <remarks>
    /// Teleporting through <c>Self.SetLocalPosition</c> alone would move the visible actor and leave the
    /// capsule behind, after which the next step drags it straight back.
    /// </remarks>
    public void Teleport(Vec3 feetPosition)
    {
        Self.SetLocalPosition(feetPosition);
        // Bring the capsule into being first if it does not exist yet, so a Teleport from OnBeginPlay
        // -- before any tick has run -- moves the character rather than only its entity. Without this
        // the two silently disagree until the first DriveWithInput.
        EnsureCapsule();
        if (_capsule != 0)
        {
            Vec3 c = feetPosition + Vec3.Up * (Height * 0.5f);
            Phys.aver_phys_character_set_position(_capsule, c.X, c.Y, c.Z);
            Phys.aver_phys_character_set_velocity(_capsule, 0f, 0f, 0f);
        }
    }

    // Release the capsule when the host unbinds this instance. Deliberately NOT done in OnEndPlay:
    // that is public and virtual, so a subclass overriding it without chaining to base would leak a
    // native capsule per character per play session -- and the first sample character in this repo
    // overrode it without chaining, which is how that stopped being hypothetical.
    internal override void OnUnbound()
    {
        if (_capsule != 0) { Phys.aver_phys_character_destroy(_capsule); _capsule = 0; }
        base.OnUnbound();
    }

    // Created on first use rather than in OnBeginPlay: a subclass that overrides OnBeginPlay and forgets
    // to call base would otherwise get a character with no body and no obvious reason why.
    private void EnsureCapsule()
    {
        if (_capsule != 0 || !Physics.Ready) return;
        Vec3 feet = Self.LocalPosition;
        Vec3 centre = feet + Vec3.Up * (Height * 0.5f);
        _capsule = Phys.aver_phys_character_create(Radius, Height, centre.X, centre.Y, centre.Z);
    }

    // The capsule's position is its CENTRE; the entity's origin is the feet.
    private void SyncFromSimulation()
    {
        float[] p = new float[3];
        if (Phys.aver_phys_character_position(_capsule, p) == 0) return;
        Self.SetLocalPosition(new Vec3(p[0], p[1], p[2] - Height * 0.5f));
    }
}
