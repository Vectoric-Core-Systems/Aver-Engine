// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The CCameraRig surface on Entity: add a rig, tune it, read its state. Design: docs/CAMERAS.md.
//
// CCameraRig is registered at RUNTIME like CMotionMatcher, so every call but AddCameraRig is a field read or write over the
// generic scene ABI, and a host that never registered it gets false, not a throw. Degrees and centimetres throughout.
// A 0 in a numeric tuning field reads as its default (arm 450, probe 12, pitch limits +-80 ...); "off" is 0 where stated.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private const string RigType = "CCameraRig";
    private const int RigFlagMask = 0x7F;   // CameraRigFlags; the high one-shot bits (apply preset, cut) belong to the system

    private bool HasRigComponent => HasComponent(RigType);

    /// <summary>Adds a CCameraRig seeded from <paramref name="preset"/> (and a CCamera). <paramref name="target"/> is the entity
    /// the arm follows; none means the rig's parent, else the rig's own entity. False for a dead entity or a build without
    /// the camera module.</summary>
    public bool AddCameraRig(CameraRigPreset preset = CameraRigPreset.Chase, Entity target = default)
    {
        try { return FwCamera.aver_fw_camera_attach(Handle, (int)preset, target.Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>True when the entity carries a rig that is not disabled.</summary>
    public bool HasCameraRig => HasRigComponent && (GetInt("CCameraRig.flags") & (int)CameraRigFlags.Disabled) == 0;

    /// <summary>Takes the rig out of play: it is never selected again (the component stays, as there is no scene call that
    /// removes a dynamically registered one). <see cref="AddCameraRig"/> brings it back. False when there is none.</summary>
    public bool RemoveCameraRig() =>
        HasRigComponent && SetInt("CCameraRig.flags", GetInt("CCameraRig.flags") | (int)CameraRigFlags.Disabled);

    /// <summary>Changes the mode; the rig blends into it over its blend time.</summary>
    public bool SetCameraRigMode(CameraMode mode) => HasRigComponent && SetInt("CCameraRig.mode", (int)mode);

    /// <summary>The entity the arm follows (none = the rig's parent, else the rig's own entity).</summary>
    public bool SetCameraRigTarget(Entity target)
    {
        if (!HasRigComponent) return false;
        try { return FwCameraScene.aver_scene_set_ref(Handle, SceneIds.Field("CCameraRig.target"), target.Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Arm length, probe radius and whether the arm collides (centimetres).</summary>
    public bool SetCameraRigArm(float lengthCm, float probeRadiusCm = 12f, bool collide = true)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.armLengthCm", lengthCm);
        SetFloat("CCameraRig.probeRadiusCm", probeRadiusCm);
        int flags = GetInt("CCameraRig.flags");
        return SetInt("CCameraRig.flags", collide ? flags & ~(int)CameraRigFlags.NoCollision : flags | (int)CameraRigFlags.NoCollision);
    }

    /// <summary>Offset at the arm's end in the camera frame: forward, right, up.</summary>
    public bool SetCameraRigSocket(Vec3 cameraFrameOffset)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.socketX", cameraFrameOffset.X);
        SetFloat("CCameraRig.socketY", cameraFrameOffset.Y);
        return SetFloat("CCameraRig.socketZ", cameraFrameOffset.Z);
    }

    /// <summary>Offset of the arm origin in world axes (eye height).</summary>
    public bool SetCameraRigPivot(Vec3 worldOffset)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.pivotX", worldOffset.X);
        SetFloat("CCameraRig.pivotY", worldOffset.Y);
        return SetFloat("CCameraRig.pivotZ", worldOffset.Z);
    }

    /// <summary>Position lag speed (1/s, 0 = none), rotation lag speed (0 = none) and the farthest the camera may trail
    /// behind its pivot (0 = unlimited).</summary>
    public bool SetCameraRigLag(float positionSpeed, float rotationSpeed = 0f, float maxDistanceCm = 0f)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.posLagSpeed", positionSpeed);
        SetFloat("CCameraRig.rotLagSpeed", rotationSpeed);
        return SetFloat("CCameraRig.lagMaxDistCm", maxDistanceCm);
    }

    /// <summary>The pivot leads by the planar velocity times <paramref name="seconds"/> (0 = off), at most <paramref name="maxCm"/>.</summary>
    public bool SetCameraRigLookAhead(float seconds, float maxCm = 150f)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.lookAheadS", seconds);
        return SetFloat("CCameraRig.lookAheadMaxCm", maxCm);
    }

    /// <summary>How far the view may look down and up, degrees (both 0 reads -80 / +80).</summary>
    public bool SetCameraRigPitchLimits(float minDegrees, float maxDegrees)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.pitchMinDeg", minDegrees);
        return SetFloat("CCameraRig.pitchMaxDeg", maxDegrees);
    }

    /// <summary>Adds look input in degrees to the rig's own yaw and pitch (Orbit, TopDown, Seat head look).</summary>
    public bool AddCameraRigLook(float yawDegrees, float pitchDegrees)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.yawDeg", GetFloat("CCameraRig.yawDeg") + yawDegrees);
        return SetFloat("CCameraRig.pitchDeg", GetFloat("CCameraRig.pitchDeg") + pitchDegrees);
    }

    /// <summary>The collision fade: the target's meshes stop drawing once the camera is nearer than <paramref name="endCm"/> to
    /// the pivot (they return at 1.15 times that). <paramref name="startCm"/> is kept for a dithered fade.</summary>
    public bool SetCameraRigFade(float startCm, float endCm)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.fadeStartCm", startCm);
        return SetFloat("CCameraRig.fadeEndCm", endCm);
    }

    /// <summary>Seconds to blend into this rig (0 = cut) and the curve.</summary>
    public bool SetCameraRigBlend(float seconds, CameraBlendCurve curve = CameraBlendCurve.EaseInOut)
    {
        if (!HasRigComponent) return false;
        SetFloat("CCameraRig.blendTimeS", seconds);
        return SetInt("CCameraRig.blendCurve", (int)curve);
    }

    /// <summary>A physics body the camera probe skips (the chassis of a vehicle). Process-local; not saved.</summary>
    public bool SetCameraRigIgnoreBody(int bodyHandle) => HasRigComponent && SetInt("CCameraRig.ignoreBody", bodyHandle);

    /// <summary>Replaces the rig's switches (collision, per-axis inherit, fade, look-at, disabled).</summary>
    public bool SetCameraRigFlags(CameraRigFlags flags)
    {
        if (!HasRigComponent) return false;
        int cur = GetInt("CCameraRig.flags");
        return SetInt("CCameraRig.flags", (cur & ~RigFlagMask) | ((int)flags & RigFlagMask));
    }

    /// <summary>What the camera system last computed for this rig. False when the entity has no rig.</summary>
    public bool TryGetCameraRigState(out CameraRigState state)
    {
        state = default;
        if (!HasRigComponent) return false;
        int bits = GetInt("CCameraRig.outState");
        state = new CameraRigState(GetFloat("CCameraRig.outArmCm"), (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0);
        return true;
    }
}
