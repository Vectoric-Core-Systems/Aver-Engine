// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_camera_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The gameplay camera C ABI (framework_camera_abi.h). Declared here, not in a bridge assembly, for the same
/// reason as <see cref="Fw"/>: only this assembly has the DllImport resolver. Every call but the two reads is deferred to
/// the camera system's next frame and applied in arrival order. Integer arguments cross as themselves; the native relay
/// packs them into the provider's float array bit for bit.</summary>
internal static class FwCamera
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_camera_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_camera_attach(int entity, int preset, int targetEntity);
    [DllImport(Lib)] internal static extern int aver_fw_camera_set_view_target(int entity, float blendSeconds, int curve, int cut);
    [DllImport(Lib)] internal static extern int aver_fw_camera_clear_view_target(float blendSeconds, int curve);
    [DllImport(Lib)] internal static extern int aver_fw_camera_push_modifier(int kind, int priority, float inS, float holdS, float outS, float a, float b, float c, float d, int tag, int flags, out int outId);
    [DllImport(Lib)] internal static extern int aver_fw_camera_remove_modifier(int id, float outSeconds);
    [DllImport(Lib)] internal static extern int aver_fw_camera_clear_modifiers(int tag);
    [DllImport(Lib)] internal static extern int aver_fw_camera_shake_at(float x, float y, float z, float radiusCm, float intensity, float falloffPower);
    [DllImport(Lib)] internal static extern int aver_fw_camera_pose(float[] outPose9, int[] outRigAndState2);
    [DllImport(Lib)] internal static extern int aver_fw_camera_rig_state(int entity, float[] outArmAndBlend2, out int outState);
    [DllImport(Lib)] internal static extern int aver_fw_camera_cut(int entity);
}

/// <summary>The Aver.Scene entry points the camera surface needs that Aver.Framework's <c>SceneNative</c> does not
/// carry: an entity-reference field (CCameraRig.target) is neither an int nor an i64 field.</summary>
internal static class FwCameraScene
{
    private const string Lib = "Aver.Scene";

    [DllImport(Lib)] internal static extern int aver_scene_get_ref(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_ref(int e, int f, int v);
}
