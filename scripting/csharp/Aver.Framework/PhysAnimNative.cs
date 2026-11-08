// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_physanim_abi.h.

using System;
using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The physical animation C ABI (framework_physanim_abi.h). Declared here, not in a bridge assembly, for the
/// same reason as <see cref="Fw"/>: only this assembly has the DllImport resolver. Strings are never passed, only
/// fnv1a64 hashes (<c>Assets.ObjectIdOf</c>). A build without the native library (a tool, a test process) answers 0
/// from every call instead of throwing, as the hair calls do.</summary>
internal static class FwPhysAnim
{
    private static class Native
    {
        private const string Lib = "Aver.Framework";

        [DllImport(Lib)] internal static extern int aver_fw_physanim_abi_version();
        [DllImport(Lib)] internal static extern int aver_fw_physanim_hit(int entity, long boneHash, float ix, float iy, float iz,
                                                                         float px, float py, float pz, float damage, float radius);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_set_profile(int entity, long profileId);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_blend(int entity, float weight, float seconds);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_simulate_subtree(int entity, long boneHash);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_set_part(int entity, long boneHash, int kind, float value);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_die(int entity);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_get_up(int entity);
        [DllImport(Lib)] internal static extern int aver_fw_physanim_state(int entity, out int life, out int facing, out int reacting);
    }

    private static int Safe(Func<int> call)
    {
        try { return call(); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    internal static int aver_fw_physanim_abi_version() => Safe(Native.aver_fw_physanim_abi_version);

    internal static int aver_fw_physanim_hit(int entity, long boneHash, float ix, float iy, float iz, float px, float py, float pz,
                                             float damage, float radius) =>
        Safe(() => Native.aver_fw_physanim_hit(entity, boneHash, ix, iy, iz, px, py, pz, damage, radius));

    internal static int aver_fw_physanim_set_profile(int entity, long profileId) => Safe(() => Native.aver_fw_physanim_set_profile(entity, profileId));
    internal static int aver_fw_physanim_blend(int entity, float weight, float seconds) => Safe(() => Native.aver_fw_physanim_blend(entity, weight, seconds));
    internal static int aver_fw_physanim_simulate_subtree(int entity, long boneHash) => Safe(() => Native.aver_fw_physanim_simulate_subtree(entity, boneHash));
    internal static int aver_fw_physanim_set_part(int entity, long boneHash, int kind, float value) => Safe(() => Native.aver_fw_physanim_set_part(entity, boneHash, kind, value));
    internal static int aver_fw_physanim_die(int entity) => Safe(() => Native.aver_fw_physanim_die(entity));
    internal static int aver_fw_physanim_get_up(int entity) => Safe(() => Native.aver_fw_physanim_get_up(entity));

    internal static int aver_fw_physanim_state(int entity, out int life, out int facing, out int reacting)
    {
        int l = 0, f = 0, r = 0;
        int has = Safe(() => Native.aver_fw_physanim_state(entity, out l, out f, out r));
        life = l; facing = f; reacting = r;
        return has;
    }
}
