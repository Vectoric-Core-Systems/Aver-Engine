// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_softbody_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The cloth and destruction C ABI (framework_softbody_abi.h). Declared here, not in a bridge assembly,
/// for the same reason as <see cref="Fw"/>: only this assembly has the DllImport resolver. Every call is
/// deferred to the soft-body system's next tick and applied in arrival order.</summary>
internal static class FwSoftBody
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_softbody_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_softbody_damage(int entity, float x, float y, float z, float dx, float dy, float dz, float radiusCm, float energyJ, int kind);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_explode(float x, float y, float z, float radiusCm, float impulseCmS, float energyJ);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_tear(int entity, float x, float y, float z, float radiusCm);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_repair(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_release(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_set_budget(int maxPieces, int maxParticles, int maxRebuilds, float debrisLifetimeS);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_set_preset(int entity, int preset);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_state(int entity, float[] outDamageMaxStrain2, int[] outPiecesBrokenState3);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_drain_events(int[] outI, float[] outF, int max);

    // ABI version 2.
    [DllImport(Lib)] internal static extern int aver_fw_softbody_set_asleep(int entity, int asleep);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_clear_debris(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_set_effects(int enabled);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_dents(int entity, float[] outMaxSetCm, int[] outDentedDebris2);
    [DllImport(Lib)] internal static extern int aver_fw_softbody_repair_all();
}
