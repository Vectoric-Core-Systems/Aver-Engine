// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for the Aver.Synapse.Abi C ABI (modules/synapse.abi/include/aver/synapse/
// synapse_ai_abi.h). Names are copied from the header character for character so the parity check
// can compare spelling.
using System.Runtime.InteropServices;

namespace Aver.Synapse;

internal static class Native
{
    private const string Lib = "Aver.Synapse.Abi";

    // ---- Crowd -----------------------------------------------------------------------------------
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_attach(int entity);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_configure(int entity, float radiusCm, float maxSpeedCm, float maxAccelCm, float priority);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_set_mode(int entity, int mode, float x, float y, float z);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_set_enabled(int entity, int enabled);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_velocity(int entity, out float outVx, out float outVy, out float outSpeed);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_set_backend(int backend);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_backend();
    [DllImport(Lib)] internal static extern IntPtr aver_syn_crowd_backend_name();
    [DllImport(Lib)] internal static extern void   aver_syn_crowd_set_gpu_backend(IntPtr icrowdBackend);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_set_max_agents(int maxAgents);
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_max_agents();
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_agent_count();
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_overflow_count();
    [DllImport(Lib)] internal static extern int    aver_syn_crowd_set_drive(int drive);

    // ---- Hearing ---------------------------------------------------------------------------------
    [DllImport(Lib)] internal static extern int aver_syn_hearing_attach(int entity);
    [DllImport(Lib)] internal static extern int aver_syn_hearing_configure(int entity, float sensitivity, float maxRangeCm, float memorySec);
    [DllImport(Lib)] internal static extern int aver_syn_emit_noise(float x, float y, float z, float loudnessCm, int tag, int sourceEntity);
    [DllImport(Lib)] internal static extern int aver_syn_hearing_get(int entity, out float outX, out float outY, out float outZ, out float outLevel, out int outTag, out float outConfidence, out float outTimeSinceSec);
    [DllImport(Lib)] internal static extern int aver_syn_hearing_forget(int entity);

    // ---- Cover -----------------------------------------------------------------------------------
    [DllImport(Lib)] internal static extern int aver_syn_cover_add(float x, float y, float dirX, float dirY, int height, float arcHalfAngleDeg);
    [DllImport(Lib)] internal static extern int aver_syn_cover_remove(int coverId);
    [DllImport(Lib)] internal static extern int aver_syn_cover_marker_attach(int entity, int height, float arcHalfAngleDeg);
    [DllImport(Lib)] internal static extern int aver_syn_cover_set_auto_generate(int on, float minSpacingCm);
    [DllImport(Lib)] internal static extern int aver_syn_cover_count();
    [DllImport(Lib)] internal static extern int aver_syn_cover_find(int seeker, float threatX, float threatY, float threatZ, float maxSeekCm, float minThreatDistCm, int requireHigh, out float outX, out float outY);
    [DllImport(Lib)] internal static extern int aver_syn_cover_release(int seeker);
    [DllImport(Lib)] internal static extern int aver_syn_cover_is_covered(int seeker, float threatX, float threatY, float threatZ);

    // ---- Squads ----------------------------------------------------------------------------------
    [DllImport(Lib)] internal static extern int aver_syn_squad_attach(int entity, int squadId, float spacingCm);
    [DllImport(Lib)] internal static extern int aver_syn_squad_set_target(int squadId, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_syn_squad_clear_target(int squadId);
    [DllImport(Lib)] internal static extern int aver_syn_squad_slot(int entity, out int outRole, out float outX, out float outY, out float outZ);
    [DllImport(Lib)] internal static extern int aver_syn_squad_spacing_push(int entity, out float outDx, out float outDy);
}
