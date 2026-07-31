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
/// <remarks>The entity's origin is the character's FEET, not the capsule centre.</remarks>
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

    /// <summary>Total capsule height including both caps, centimetres.</summary>
    public float Height = 180f;

    /// <summary>Capsule radius, centimetres. Must be less than half of <see cref="Height"/>.</summary>
    public float Radius = 34f;

    /// <summary>Upward speed a jump starts with, cm/s.</summary>
    public float JumpSpeed = 465f;

    /// <summary>How far the view may look down / up, degrees.</summary>
    public float PitchMin = -85f, PitchMax = 85f;

    private float _yaw;         // degrees
    private float _pitch;       // degrees
    private int   _capsule;     // physics character handle; 0 when none
    private Entity _view;

    /// <summary>The character's head: a child entity at eye height carrying the view pitch.</summary>
    public Entity View => _view;

    /// <summary>The character's facing yaw, degrees.</summary>
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

    /// <summary>The character's current velocity, cm/s.</summary>
    public Vec3 Velocity
    {
        get
        {
            if (_capsule == 0) return Vec3.Zero;
            float[] v = new float[3];
            return Phys.aver_phys_character_velocity(_capsule, v) != 0 ? new Vec3(v[0], v[1], v[2]) : Vec3.Zero;
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

    /// <summary>Runs one frame of character control from supplied move and look values.</summary>
    /// <param name="moveAxis">X = forward intent, Y = right intent, each -1..1.</param>
    protected void Drive(float dt, Vec3 moveAxis, float yawDeltaDeg, float pitchDeltaDeg)
    {
        Fw.aver_fw_set_view((int)CameraViewMode, EyeHeight, BoomLength);
        EnsureView();

        _yaw += yawDeltaDeg;
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

    /// <summary>Snaps the facing yaw, degrees.</summary>
    protected void SetYaw(float yawDegrees)
    {
        _yaw = yawDegrees;
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
    private void ApplyLookRotation()
    {
        Self.SetLocalRotation(new Rot(_yaw, 0f, 0f).ToQuat());
        if (_view.IsAlive) _view.SetLocalRotation(new Rot(0f, _pitch, 0f).ToQuat());
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
    }

    // Copies the settled capsule centre back to the entity origin (the feet).
    private void SyncFromSimulation()
    {
        float[] p = new float[3];
        if (Phys.aver_phys_character_position(_capsule, p) == 0) return;
        Self.SetLocalPosition(new Vec3(p[0], p[1], p[2] - Height * 0.5f));
    }
}
