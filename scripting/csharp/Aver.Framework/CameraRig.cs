// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Gameplay cameras from a script: the view target, the modifier stack and the pose read. The per-rig members are on Entity
// (EntityCameraRig.cs). Design: docs/CAMERAS.md.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>How a rig places the camera. Pinned to <c>CameraMode</c> in CameraTypes.hpp and AVER_FW_CAM_MODE_*.</summary>
public enum CameraMode
{
    /// <summary>Behind the target on a spring arm, collision-swept, following the target's heading.</summary>
    Chase = 0,
    /// <summary>On a spring arm around the target, turned by look input rather than by the target.</summary>
    Orbit = 1,
    /// <summary>At the view node (the eyes), no arm and no lag; the flagged body hides.</summary>
    FirstPerson = 2,
    /// <summary>On the rig entity's own transform (a child of a vehicle at the seat) with a limited head look.</summary>
    Seat = 3,
    /// <summary>High above the target at a fixed angle.</summary>
    TopDown = 4,
    /// <summary>On the rig entity's transform, optionally turning to face the target.</summary>
    Fixed = 5,
}

/// <summary>The starting values of a rig. Pinned to <c>CameraPreset</c> and AVER_FW_CAM_PRESET_*.</summary>
public enum CameraRigPreset { Chase = 0, VehicleChase = 1, Orbit = 2, FirstPerson = 3, Seat = 4, TopDown = 5, Fixed = 6 }

/// <summary>The curve of a view-target blend. Pinned to <c>BlendCurve</c> and AVER_FW_CAM_CURVE_*.</summary>
public enum CameraBlendCurve { EaseInOut = 0, Linear = 1, EaseIn = 2, EaseOut = 3 }

/// <summary>What a camera modifier changes. Pinned to <c>ModKind</c> and AVER_FW_CAM_MOD_*.</summary>
public enum CameraModifierKind { FovKick = 0, FovScale = 1, Punch = 2, RollLean = 3, ArmScale = 4, SocketOffset = 5 }

/// <summary>Rig switches, negative sense (zero is a working Chase rig). Pinned to the kRig* bits in CameraTypes.hpp.</summary>
[Flags]
public enum CameraRigFlags
{
    NoCollision = 1,
    NoInheritYaw = 2,
    NoInheritPitch = 4,
    NoInheritRoll = 8,
    NoFade = 16,
    LookAtTarget = 32,
    Disabled = 64,
}

/// <summary>The camera after the active rig and the modifier stack, before the player's shake and bob.</summary>
public readonly record struct CameraPose(Vec3 Position, float YawDegrees, float PitchDegrees, float RollDegrees,
                                         float FovDegrees, float NearCm, float FarCm);

/// <summary>One rig's runtime state, as the system last wrote it.</summary>
public readonly record struct CameraRigState(float ArmCm, bool Active, bool Blending, bool Colliding, bool Faded);

/// <summary>The camera system: which rig is the view, blends between rigs, the modifier stack, and shake with distance.</summary>
/// <remarks>Every call answers false or 0, and never throws, in a process with no engine or a host built without the camera
/// module. The camera system applies calls in arrival order at the top of its next frame.</remarks>
public static class CameraRig
{
    /// <summary>Raised (at the new view target, or the pawn when none) when the live rig changes.</summary>
    public const string EventViewTargetChanged = "CameraViewTargetChanged";
    /// <summary>Raised when a blend into the live rig completes.</summary>
    public const string EventBlendFinished = "CameraBlendFinished";
    /// <summary>Raised when the camera teleports (a cut, or a lag snap).</summary>
    public const string EventCut = "CameraCut";

    private const float RadToDeg = 180f / MathF.PI;

    /// <summary>Makes <paramref name="rig"/> the view. Overrides automatic selection until <see cref="ClearViewTarget"/>.
    /// <see cref="Entity.None"/> returns to the legacy pawn camera. <paramref name="cut"/> skips the blend.</summary>
    public static bool SetViewTarget(Entity rig, float blendSeconds = 0.5f, CameraBlendCurve curve = CameraBlendCurve.EaseInOut, bool cut = false)
    {
        try { return FwCamera.aver_fw_camera_set_view_target(rig.Handle, blendSeconds, (int)curve, cut ? 1 : 0) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Back to automatic selection: the best rig under the possessed pawn, else the legacy camera.</summary>
    public static bool ClearViewTarget(float blendSeconds = 0.5f, CameraBlendCurve curve = CameraBlendCurve.EaseInOut)
    {
        try { return FwCamera.aver_fw_camera_clear_view_target(blendSeconds, (int)curve) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Snaps the lag state of <paramref name="rig"/> (none = the active rig): the camera jumps to where it would be
    /// at rest, with no blend and no trail.</summary>
    public static bool Cut(Entity rig = default)
    {
        try { return FwCamera.aver_fw_camera_cut(rig.Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>The rig that is the view now, or <see cref="Entity.None"/> for the legacy pawn camera.</summary>
    public static Entity ActiveRig
    {
        get
        {
            var rigAndState = new int[2];
            try { return FwCamera.aver_fw_camera_pose(new float[9], rigAndState) != 0 ? new Entity(rigAndState[0]) : Entity.None; }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return Entity.None; }
        }
    }

    /// <summary>True while a blend between views is running.</summary>
    public static bool IsBlending
    {
        get
        {
            var rigAndState = new int[2];
            try { return FwCamera.aver_fw_camera_pose(new float[9], rigAndState) != 0 && (rigAndState[1] & 1) != 0; }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
        }
    }

    /// <summary>The camera pose after the rig and the modifier stack (before the player's shake and bob). False before the
    /// camera system has produced one.</summary>
    public static bool TryGetPose(out CameraPose pose)
    {
        pose = default;
        var p = new float[9];
        try
        {
            if (FwCamera.aver_fw_camera_pose(p, new int[2]) == 0) return false;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
        pose = new CameraPose(new Vec3(p[0], p[1], p[2]), p[3] * RadToDeg, p[4] * RadToDeg, p[5] * RadToDeg, p[6] * RadToDeg, p[7], p[8]);
        return true;
    }

    // ---- modifiers: every call returns an id (0 on refusal) to pass to RemoveModifier --------------------------------

    /// <summary>A FOV kick: <paramref name="deltaDegrees"/> added, ramping in, holding, ramping out. Gated by Reduce motion.</summary>
    public static int PushFovKick(float deltaDegrees, float inSeconds = 0.05f, float holdSeconds = 0.1f, float outSeconds = 0.4f, int tag = 0) =>
        Push(CameraModifierKind.FovKick, inSeconds, holdSeconds, outSeconds, deltaDegrees, 0f, 0f, tag);

    /// <summary>Zoom: the FOV is multiplied by <paramref name="multiplier"/> (0.6 zooms in), held until removed. The player's own
    /// action, so Reduce motion does not gate it.</summary>
    public static int PushFovScale(float multiplier, float blendSeconds = 0.2f, int tag = 0) =>
        Push(CameraModifierKind.FovScale, blendSeconds, -1f, blendSeconds, multiplier, 0f, 0f, tag);

    /// <summary>A recoil: a quick rotation offset that eases back over <paramref name="recoverSeconds"/>. Gated by Reduce motion.</summary>
    public static int PushPunch(float pitchDegrees, float yawDegrees = 0f, float rollDegrees = 0f, float recoverSeconds = 0.3f, int tag = 0) =>
        Push(CameraModifierKind.Punch, 0.03f, 0f, recoverSeconds, pitchDegrees, yawDegrees, rollDegrees, tag);

    /// <summary>A roll lean in degrees, held until removed. Gated by Reduce motion.</summary>
    public static int PushRoll(float degrees, float blendSeconds = 0.3f, int tag = 0) =>
        Push(CameraModifierKind.RollLean, blendSeconds, -1f, blendSeconds, degrees, 0f, 0f, tag);

    /// <summary>The spring arm's length is multiplied by <paramref name="factor"/> (zoom out for speed), held until removed.</summary>
    public static int PushArmScale(float factor, float blendSeconds = 0.3f, int tag = 0) =>
        Push(CameraModifierKind.ArmScale, blendSeconds, -1f, blendSeconds, factor, 0f, 0f, tag);

    /// <summary>An offset at the arm's end in the camera frame (forward, right, up), held until removed: over-the-shoulder aim.</summary>
    public static int PushSocketOffset(Vec3 cameraFrameOffset, float blendSeconds = 0.2f, int tag = 0) =>
        Push(CameraModifierKind.SocketOffset, blendSeconds, -1f, blendSeconds, cameraFrameOffset.X, cameraFrameOffset.Y, cameraFrameOffset.Z, tag);

    /// <summary>Starts a modifier's blend out from its current strength (<paramref name="outSeconds"/> &lt; 0 uses its own).</summary>
    public static bool RemoveModifier(int id, float outSeconds = -1f)
    {
        try { return FwCamera.aver_fw_camera_remove_modifier(id, outSeconds) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Blends out every modifier with <paramref name="tag"/> (0 = all). Returns how many.</summary>
    public static int ClearModifiers(int tag = 0)
    {
        try { return FwCamera.aver_fw_camera_clear_modifiers(tag); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    private static int Push(CameraModifierKind kind, float inS, float holdS, float outS, float a, float b, float c, int tag)
    {
        try { return FwCamera.aver_fw_camera_push_modifier((int)kind, 0, inS, holdS, outS, a, b, c, 0f, tag, 0, out int id) != 0 ? id : 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    // ---- shake ---------------------------------------------------------------------------------------------------------

    /// <summary>Camera shake, 0..1: the existing <see cref="CameraFx"/> trauma (the player's shake and Reduce motion options apply).</summary>
    public static void AddShake(float trauma) => CameraFx.AddShake(trauma);

    /// <summary>Shake that fades with distance from the current camera: intensity * (1 - d/radius)^falloffPower.</summary>
    public static bool ShakeAt(Vec3 worldPosition, float radiusCm, float intensity, float falloffPower = 2f)
    {
        try { return FwCamera.aver_fw_camera_shake_at(worldPosition.X, worldPosition.Y, worldPosition.Z, radiusCm, intensity, falloffPower) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    // ---- orbit control --------------------------------------------------------------------------------------------------

    /// <summary>One frame of orbit control from the mouse: look input scaled by <paramref name="degreesPerPixel"/> and the
    /// wheel as an arm change. Call it from a tick while the orbit rig is the view.</summary>
    public static void DriveOrbitFromMouse(Entity rig, float degreesPerPixel = 0.15f, float wheelCmPerNotch = 40f)
    {
        try
        {
            float dx = Input.MouseDeltaX, dy = Input.MouseDeltaY, wheel = Input.MouseWheel;
            if (dx != 0f || dy != 0f) rig.AddCameraRigLook(dx * degreesPerPixel, -dy * degreesPerPixel);
            if (wheel != 0f && rig.HasCameraRig)
            {
                float arm = rig.GetFloat("CCameraRig.armLengthCm");
                if (arm <= 0f) arm = 450f;
                rig.SetCameraRigArm(MathF.Max(50f, arm - wheel * wheelCmPerNotch), rig.GetFloat("CCameraRig.probeRadiusCm"),
                                    (rig.GetInt("CCameraRig.flags") & (int)CameraRigFlags.NoCollision) == 0);
            }
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
    }

    /// <summary>The mode a preset starts in (VehicleChase is a Chase rig).</summary>
    public static CameraMode ModeOf(CameraRigPreset preset) => preset switch
    {
        CameraRigPreset.Orbit => CameraMode.Orbit,
        CameraRigPreset.FirstPerson => CameraMode.FirstPerson,
        CameraRigPreset.Seat => CameraMode.Seat,
        CameraRigPreset.TopDown => CameraMode.TopDown,
        CameraRigPreset.Fixed => CameraMode.Fixed,
        _ => CameraMode.Chase,
    };
}
