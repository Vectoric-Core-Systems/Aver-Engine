// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Pose interpolation: which entities are drawn between their last two fixed steps (framework_interp_abi.h).

using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>The pose interpolation C ABI (framework_interp_abi.h). Names and parameter names are copied from the header
/// character for character (tests/abi checks them).</summary>
internal static class FwInterp
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_interp_abi_version();
    [DllImport(Lib)] internal static extern void aver_fw_interp_set_enabled(int enabled);
    [DllImport(Lib)] internal static extern int aver_fw_interp_enabled();
    [DllImport(Lib)] internal static extern void aver_fw_interp_set_mode(int entity, int mode);
    [DllImport(Lib)] internal static extern int aver_fw_interp_mode(int entity);
    [DllImport(Lib)] internal static extern void aver_fw_interp_reset(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_interp_pose(int entity, float[]? outPosition3, float[]? outRotation4);
}

/// <summary>How an entity takes part in pose interpolation.</summary>
public enum InterpolationMode
{
    /// <summary>Follows the switch: a dynamic rigid body is blended when it is on, anything else is not.</summary>
    Inherit = 0,
    /// <summary>Blended whatever the switch says.</summary>
    On = 1,
    /// <summary>Never blended, a rigid body included.</summary>
    Off = 2,
}

/// <summary>Draws entities between their last two physics steps. The project's PHYSICS.INTERPOLATION switches it on for dynamic rigid
/// bodies; this puts any other entity on the same footing (move it in the physics tick), takes one out, teleports without a blend and reads
/// the pose that is drawn. Nothing here changes how the simulation runs.</summary>
public static class PoseInterpolation
{
    /// <summary>The switch every entity that inherits follows: null leaves it to the project, true or false overrides it.</summary>
    public static bool? Enabled
    {
        get { int v = FwInterp.aver_fw_interp_enabled(); return v < 0 ? null : v != 0; }
        set => FwInterp.aver_fw_interp_set_enabled(value is null ? -1 : (value.Value ? 1 : 0));
    }

    public static InterpolationMode GetMode(int entity) => (InterpolationMode)FwInterp.aver_fw_interp_mode(entity);
    public static void SetMode(int entity, InterpolationMode mode) => FwInterp.aver_fw_interp_set_mode(entity, (int)mode);

    /// <summary>The next pose of the entity is a teleport: it is drawn there at once, with no blend from where it was.</summary>
    public static void Reset(int entity) => FwInterp.aver_fw_interp_reset(entity);

    /// <summary>The pose last drawn for a blended entity: world position (engine cm) and rotation (x y z w). False when it has none.</summary>
    public static bool TryGetDrawnPose(int entity, out Vec3 position, out Quat rotation)
    {
        var p = new float[3]; var q = new float[4];
        if (FwInterp.aver_fw_interp_pose(entity, p, q) == 0) { position = default; rotation = Quat.Identity; return false; }
        position = new Vec3(p[0], p[1], p[2]);
        rotation = new Quat(q[0], q[1], q[2], q[3]);
        return true;
    }
}
