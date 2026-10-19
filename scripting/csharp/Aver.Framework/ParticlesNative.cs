// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_particles_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The particle C ABI (framework_particles_abi.h). Declared here, not in a bridge assembly, for the same reason
/// as <see cref="Fw"/>: only this assembly has the DllImport resolver. Strings are never passed, only fnv1a64 hashes
/// (<c>Assets.ObjectIdOf</c>); with no provider installed every call answers 0.</summary>
internal static class FwParticles
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_particles_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_particles_spawn_at(long effect, float px, float py, float pz,
                                                                          float dx, float dy, float dz, int count);
    [DllImport(Lib)] internal static extern int aver_fw_particles_spawn_at_ex(long effect, float[] args16, int count);
    [DllImport(Lib)] internal static extern int aver_fw_particles_set_param(int entity, long nameHash, int kind,
                                                                           float v0, float v1, float v2, float v3);
    [DllImport(Lib)] internal static extern int aver_fw_particles_burst(int entity, int count);
    [DllImport(Lib)] internal static extern int aver_fw_particles_stop(int entity, int killParticles);
    [DllImport(Lib)] internal static extern int aver_fw_particles_set_modifiers(int entity, float rateScale, float sizeScale, float speedScale);
    [DllImport(Lib)] internal static extern int aver_fw_particles_emitter_state(int entity, out int alive, out int tier);
    [DllImport(Lib)] internal static extern int aver_fw_particles_stats(int[] outSix);
    [DllImport(Lib)] internal static extern int aver_fw_particles_set_budget(int maxGpu, int maxCpu, int maxSpawnAtPerFrame);
    [DllImport(Lib)] internal static extern int aver_fw_particles_set_gpu_mode(int mode);
}
