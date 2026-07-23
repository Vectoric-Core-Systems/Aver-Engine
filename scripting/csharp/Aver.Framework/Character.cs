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
/// kinematic WASD + mouse control: <see cref="DriveWithInput"/> turns the character with the mouse and
/// walks it in its own facing plane. The editor reads <see cref="CameraViewMode"/>, <see cref="EyeHeight"/>
/// and <see cref="BoomLength"/> off the possessed character to place the play camera.
/// </summary>
/// <remarks>
/// There is no physics yet, so movement is a direct translate on the ground plane — a character does not
/// fall, collide or step. A collision/gravity pass slots under <see cref="DriveWithInput"/> later without
/// changing this surface: it stays the place a character reads input and expresses intent to move.
/// </remarks>
public abstract class AverCharacter : AverPawn
{
    /// <summary>Ground move speed, centimetres per second.</summary>
    public float MoveSpeed = 350f;

    /// <summary>Mouse-look sensitivity, degrees of yaw per pixel of mouse movement.</summary>
    public float TurnSpeed = 0.2f;

    /// <summary>Which view the camera should use while this character is possessed.</summary>
    public CameraView CameraViewMode = CameraView.ThirdPerson;

    /// <summary>First-person eye height above the character's origin, centimetres.</summary>
    public float EyeHeight = 160f;

    /// <summary>Third-person camera distance behind the character, centimetres.</summary>
    public float BoomLength = 450f;

    private float _yaw;   // facing in degrees, turned by the mouse

    /// <summary>The character's facing yaw, degrees. Turned by <see cref="DriveWithInput"/>.</summary>
    public float Yaw => _yaw;

    /// <summary>
    /// Standard character control for one frame: turn with the mouse (yaw about up), then walk with WASD /
    /// arrows in the facing plane at <see cref="MoveSpeed"/>. Call from <c>OnTick</c> while this character is
    /// the possessed pawn. Kinematic — it translates directly, with no collision yet.
    /// </summary>
    protected void DriveWithInput(float dt)
    {
        // Publish the camera this character wants, so the editor's play view can follow it.
        Fw.aver_fw_set_view((int)CameraViewMode, EyeHeight, BoomLength);

        _yaw += Input.MouseDeltaX * TurnSpeed;
        Self.SetLocalRotation(new Rot(_yaw, 0f, 0f).ToQuat());   // yaw about +Z (up)

        Vec3 axis = Input.MoveAxis;   // X = forward intent, Y = right intent
        if (axis.X != 0f || axis.Y != 0f)
        {
            Vec3 move = Self.Forward * axis.X + Self.Right * axis.Y;
            float len = MathF.Sqrt(move.X * move.X + move.Y * move.Y + move.Z * move.Z);
            if (len > 1e-4f) Self.Translate(move * (MoveSpeed * dt / len));   // normalise so a diagonal is not faster
        }
    }

    /// <summary>Snap the facing yaw (degrees) — e.g. to spawn a character facing a direction without the
    /// mouse having moved it there.</summary>
    protected void SetYaw(float yawDegrees)
    {
        _yaw = yawDegrees;
        Self.SetLocalRotation(new Rot(_yaw, 0f, 0f).ToQuat());
    }
}
