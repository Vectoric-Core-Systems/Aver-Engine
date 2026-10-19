// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the Camera Aver Node nodes call (scripting/csharp/Aver.Graph/GraphCameraNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsCamera.hpp). Each is a thin wrapper over the public members in CameraRig.cs and
// EntityCameraRig.cs. A node never throws: a dead entity, a missing rig or a missing native library answers false/0, logged
// once per node type (FamiliesBLog).

using Aver.Scene;

namespace Aver.Framework;

internal static class CameraGraph
{
    private const string NoRig = "the entity is dead or has no camera rig";
    private const string NoCamera = "the camera system is not running";

    /// <summary>AN_CameraAddRig: attaches a rig seeded from the preset (0 Chase, 1 VehicleChase, 2 Orbit, 3 FirstPerson, 4 Seat, 5 TopDown, 6 Fixed).</summary>
    internal static bool AddRigForGraph(int entity, int preset, int target)
    {
        try { return FamiliesBLog.Ok("AN_CameraAddRig", new Entity(entity).AddCameraRig((CameraRigPreset)preset, new Entity(target)), "the entity is dead, the preset is unknown or the camera module is not built in"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraAddRig", e); }
    }

    /// <summary>AN_CameraSetMode: 0 Chase, 1 Orbit, 2 FirstPerson, 3 Seat, 4 TopDown, 5 Fixed; blends over the rig's blend time.</summary>
    internal static bool SetModeForGraph(int entity, int mode)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetMode", new Entity(entity).SetCameraRigMode((CameraMode)mode), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetMode", e); }
    }

    /// <summary>AN_CameraSetTarget: the entity the arm follows (0 = the rig's parent, else the rig's own entity).</summary>
    internal static bool SetTargetForGraph(int entity, int target)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetTarget", new Entity(entity).SetCameraRigTarget(new Entity(target)), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetTarget", e); }
    }

    /// <summary>AN_CameraSetViewTarget: makes the rig the view (0 = the legacy pawn camera), blending over the time and curve.</summary>
    internal static bool SetViewTargetForGraph(int entity, float blendSeconds, int curve)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetViewTarget", CameraRig.SetViewTarget(new Entity(entity), blendSeconds, (CameraBlendCurve)curve), "the entity has no camera rig, or the camera system is not running"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetViewTarget", e); }
    }

    /// <summary>AN_CameraClearViewTarget: back to the best rig under the possessed pawn.</summary>
    internal static bool ClearViewTargetForGraph(float blendSeconds, int curve)
    {
        try { return FamiliesBLog.Ok("AN_CameraClearViewTarget", CameraRig.ClearViewTarget(blendSeconds, (CameraBlendCurve)curve), NoCamera); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraClearViewTarget", e); }
    }

    /// <summary>AN_CameraCut: snaps the rig's lag state (0 = the active rig).</summary>
    internal static bool CutForGraph(int entity)
    {
        try { return FamiliesBLog.Ok("AN_CameraCut", CameraRig.Cut(new Entity(entity)), "the entity has no camera rig, or the camera system is not running"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraCut", e); }
    }

    /// <summary>AN_CameraSetArm: arm length and probe radius in centimetres, and whether the arm collides.</summary>
    internal static bool SetArmForGraph(int entity, float lengthCm, float probeRadiusCm, bool collide)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetArm", new Entity(entity).SetCameraRigArm(lengthCm, probeRadiusCm, collide), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetArm", e); }
    }

    /// <summary>AN_CameraSetSocket: offset at the arm's end in the camera frame (forward, right, up).</summary>
    internal static bool SetSocketForGraph(int entity, float x, float y, float z)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetSocket", new Entity(entity).SetCameraRigSocket(new Vec3(x, y, z)), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetSocket", e); }
    }

    /// <summary>AN_CameraSetPivot: offset of the arm origin in world axes (eye height).</summary>
    internal static bool SetPivotForGraph(int entity, float x, float y, float z)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetPivot", new Entity(entity).SetCameraRigPivot(new Vec3(x, y, z)), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetPivot", e); }
    }

    /// <summary>AN_CameraSetLag: position and rotation lag speeds (1/s, 0 = none) and the farthest the camera may trail (0 = unlimited).</summary>
    internal static bool SetLagForGraph(int entity, float posSpeed, float rotSpeed, float maxDistCm)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetLag", new Entity(entity).SetCameraRigLag(posSpeed, rotSpeed, maxDistCm), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetLag", e); }
    }

    /// <summary>AN_CameraSetLookAhead: the pivot leads by planar velocity times seconds (0 = off), at most maxCm.</summary>
    internal static bool SetLookAheadForGraph(int entity, float seconds, float maxCm)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetLookAhead", new Entity(entity).SetCameraRigLookAhead(seconds, maxCm), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetLookAhead", e); }
    }

    /// <summary>AN_CameraSetLimits: how far the view may look down and up, degrees.</summary>
    internal static bool SetLimitsForGraph(int entity, float pitchMinDeg, float pitchMaxDeg)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetLimits", new Entity(entity).SetCameraRigPitchLimits(pitchMinDeg, pitchMaxDeg), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetLimits", e); }
    }

    /// <summary>AN_CameraAddLook: adds look input in degrees to the rig's own yaw and pitch.</summary>
    internal static bool AddLookForGraph(int entity, float yawDeg, float pitchDeg)
    {
        try { return FamiliesBLog.Ok("AN_CameraAddLook", new Entity(entity).AddCameraRigLook(yawDeg, pitchDeg), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraAddLook", e); }
    }

    /// <summary>AN_CameraSetFade: the target's meshes hide when the camera is nearer than endCm to the pivot.</summary>
    internal static bool SetFadeForGraph(int entity, float startCm, float endCm)
    {
        try { return FamiliesBLog.Ok("AN_CameraSetFade", new Entity(entity).SetCameraRigFade(startCm, endCm), NoRig); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraSetFade", e); }
    }

    /// <summary>AN_CameraPushFovKick: a FOV kick in degrees; returns the modifier id (0 on refusal).</summary>
    internal static int PushFovKickForGraph(float deltaDeg, float inSeconds, float holdSeconds, float outSeconds)
    {
        try { return Positive("AN_CameraPushFovKick", CameraRig.PushFovKick(deltaDeg, inSeconds, holdSeconds, outSeconds)); }
        catch (Exception e) { FamiliesBLog.Failed("AN_CameraPushFovKick", e); return 0; }
    }

    /// <summary>AN_CameraPushZoom: the FOV is multiplied by fovScale until the modifier is removed; returns its id.</summary>
    internal static int PushZoomForGraph(float fovScale, float blendSeconds)
    {
        try { return Positive("AN_CameraPushZoom", CameraRig.PushFovScale(fovScale, blendSeconds)); }
        catch (Exception e) { FamiliesBLog.Failed("AN_CameraPushZoom", e); return 0; }
    }

    /// <summary>AN_CameraPunch: a recoil of pitch, yaw and roll degrees that recovers over recoverSeconds; returns its id.</summary>
    internal static int PunchForGraph(float pitchDeg, float yawDeg, float rollDeg, float recoverSeconds)
    {
        try { return Positive("AN_CameraPunch", CameraRig.PushPunch(pitchDeg, yawDeg, rollDeg, recoverSeconds)); }
        catch (Exception e) { FamiliesBLog.Failed("AN_CameraPunch", e); return 0; }
    }

    /// <summary>AN_CameraRemoveModifier: starts the modifier's blend out (outSeconds &lt; 0 uses its own).</summary>
    internal static bool RemoveModifierForGraph(int id, float outSeconds)
    {
        try { return FamiliesBLog.Ok("AN_CameraRemoveModifier", CameraRig.RemoveModifier(id, outSeconds), "no such modifier, or the camera system is not running"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraRemoveModifier", e); }
    }

    /// <summary>AN_CameraClearModifiers: blends out every modifier with the tag (0 = all); returns how many.</summary>
    internal static int ClearModifiersForGraph(int tag)
    {
        try { return CameraRig.ClearModifiers(tag); }
        catch (Exception e) { FamiliesBLog.Failed("AN_CameraClearModifiers", e); return 0; }
    }

    /// <summary>AN_CameraShakeAt: shake with falloff 2 that fades with distance from the camera.</summary>
    internal static bool ShakeAtForGraph(float x, float y, float z, float radiusCm, float intensity)
    {
        try { return FamiliesBLog.Ok("AN_CameraShakeAt", CameraRig.ShakeAt(new Vec3(x, y, z), radiusCm, intensity), NoCamera); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraShakeAt", e); }
    }

    /// <summary>AN_CameraGetPose: the camera after the rig and modifiers (degrees); found is false before the first frame.</summary>
    internal static bool GetPoseForGraph(out float x, out float y, out float z, out float yawDeg, out float pitchDeg, out float rollDeg, out float fovDeg)
    {
        x = y = z = yawDeg = pitchDeg = rollDeg = fovDeg = 0f;
        try
        {
            if (!CameraRig.TryGetPose(out CameraPose p)) return false;
            x = p.Position.X; y = p.Position.Y; z = p.Position.Z;
            yawDeg = p.YawDegrees; pitchDeg = p.PitchDegrees; rollDeg = p.RollDegrees; fovDeg = p.FovDegrees;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraGetPose", e); }
    }

    /// <summary>AN_CameraGetRigState: the rig's resolved arm length and its state flags; found is false without a rig.</summary>
    internal static bool GetRigStateForGraph(int entity, out float armCm, out bool colliding, out bool active, out bool blending)
    {
        armCm = 0f; colliding = active = blending = false;
        try
        {
            if (!new Entity(entity).TryGetCameraRigState(out CameraRigState s)) return false;
            armCm = s.ArmCm; colliding = s.Colliding; active = s.Active; blending = s.Blending;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_CameraGetRigState", e); }
    }

    /// <summary>AN_CameraGetViewTarget: the rig that is the view (0 = the legacy camera), whether a blend is running and how far.</summary>
    internal static int GetViewTargetForGraph(out bool blending, out float blendAlpha)
    {
        blending = false;
        blendAlpha = 0f;
        try
        {
            Entity rig = CameraRig.ActiveRig;
            blending = CameraRig.IsBlending;
            if (rig.IsValid)
            {
                var f = new float[2];
                if (FwCamera.aver_fw_camera_rig_state(rig.Handle, f, out _) != 0) blendAlpha = f[1];
            }
            else
            {
                blendAlpha = blending ? 0f : 1f;   // no rig to ask: the legacy camera has no alpha to report
            }
            return rig.Handle;
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_CameraGetViewTarget", e); return 0; }
    }

    private static int Positive(string node, int id)
    {
        if (id == 0) FamiliesBLog.Once(node, $"[{node}] the modifier stack is full, or the camera system is not running");
        return id;
    }
}
