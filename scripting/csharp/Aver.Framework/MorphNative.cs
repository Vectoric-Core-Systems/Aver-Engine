// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_morph_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The blend-shape / facial-animation C ABI (framework_morph_abi.h). Declared here, not in a bridge
/// assembly, for the same reason as <see cref="Fw"/>: only this assembly has the DllImport resolver. Strings are
/// never passed, only fnv1a64 hashes (<c>Assets.ObjectIdOf</c>).</summary>
internal static class FwMorph
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_morph_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_morph_set_target(int entity, long target, float weight, float blendSeconds);
    [DllImport(Lib)] internal static extern int aver_fw_morph_set_pose(int entity, long pose, float weight, float blendSeconds);
    [DllImport(Lib)] internal static extern int aver_fw_morph_clear(int entity, long name, int kindMask, float blendSeconds);
    [DllImport(Lib)] internal static extern int aver_fw_morph_get_weight(int entity, long target, out float weight);
    [DllImport(Lib)] internal static extern int aver_fw_morph_set_rig(int entity, long rigAsset);

    [DllImport(Lib)] internal static extern int aver_fw_morph_lipsync_play(int entity, long track, float rate, float startSeconds, float strength);
    [DllImport(Lib)] internal static extern int aver_fw_morph_lipsync_stop(int entity, float blendOutSeconds);
    [DllImport(Lib)] internal static extern int aver_fw_morph_lipsync_set_time(int entity, float seconds);
    [DllImport(Lib)] internal static extern int aver_fw_morph_lipsync_queue(int entity, long symbol, float startSeconds, float durationSeconds, float intensity);
    [DllImport(Lib)] internal static extern int aver_fw_morph_lipsync_state(int entity, float[] outTimeLengthStrength);

    [DllImport(Lib)] internal static extern int aver_fw_morph_info(int entity, long[] outI3, float[] outF1);
}
