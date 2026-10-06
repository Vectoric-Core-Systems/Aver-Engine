// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The graph-facing surface of the AI systems: one method per AN_ node, with the primitive-only
// signature the graph compiler's reflection wrappers expect (see Aver.Framework's GraphInterop:
// each AN_xxx node is reached through a GraphInterop.XxxForGraph that forwards here). Every method
// returns true when it did its job; "nothing to report" outcomes are out values, not failures.
namespace Aver.Synapse;

/// <summary>The bodies of the AN_ crowd, hearing, cover and squad nodes.</summary>
public static class SynapseAiGraph
{
    // ---- AN_CrowdSetAgent (exec) ---------------------------------------------------------------------
    /// <summary>Attaches the crowd component if the entity lacks one, then applies the settings.</summary>
    public static bool CrowdSetAgentForGraph(int entity, float radiusCm, float maxSpeedCm, float maxAccelCm, float priority)
    {
        _ = Native.aver_syn_crowd_attach(entity);   // already attached is fine
        return Native.aver_syn_crowd_configure(entity, radiusCm, maxSpeedCm, maxAccelCm, priority) != 0;
    }

    // ---- AN_CrowdSetMode (exec) ----------------------------------------------------------------------
    public static bool CrowdSetModeForGraph(int entity, int mode, float x, float y, float z)
        => Native.aver_syn_crowd_set_mode(entity, mode, x, y, z) != 0;

    // ---- AN_CrowdSetBackend (exec) -------------------------------------------------------------------
    /// <summary>backend: 0 CPU, 1 GPU. maxAgents &lt; 0 leaves the cap alone.</summary>
    public static bool CrowdSetBackendForGraph(int backend, int maxAgents)
    {
        bool ok = Native.aver_syn_crowd_set_backend(backend) != 0;
        if (maxAgents >= 0) ok &= Native.aver_syn_crowd_set_max_agents(maxAgents) != 0;
        return ok;
    }

    // ---- AN_GetCrowdVelocity (pure) ------------------------------------------------------------------
    public static bool GetCrowdVelocityForGraph(int entity, out float vx, out float vy, out float speed)
        => Native.aver_syn_crowd_velocity(entity, out vx, out vy, out speed) != 0;

    // ---- AN_CrowdSteer (pure) ------------------------------------------------------------------------
    /// <summary>
    /// Turns a crowd velocity into the forward/right/yawDelta CharacterMove consumes. forward and right
    /// are the velocity's components along the character's facing and its right, scaled to -1..1 by
    /// <paramref name="maxSpeedCm"/>; yawDelta turns toward the velocity, at most
    /// <paramref name="turnRateDegPerSec"/> * dt degrees. fwdX/fwdY is the character's facing on the ground.
    /// </summary>
    public static void SteerFromVelocity(float vx, float vy, float fwdX, float fwdY, float maxSpeedCm, float dt,
                                         float turnRateDegPerSec, out float forward, out float right, out float yawDelta)
    {
        forward = 0f; right = 0f; yawDelta = 0f;
        float fl = MathF.Sqrt(fwdX * fwdX + fwdY * fwdY);
        if (fl < 1e-6f || maxSpeedCm <= 0f) return;
        fwdX /= fl; fwdY /= fl;

        // Right is facing rotated toward +Y: engine space is X forward, Y right.
        float rx = -fwdY, ry = fwdX;
        forward = Math.Clamp((vx * fwdX + vy * fwdY) / maxSpeedCm, -1f, 1f);
        right = Math.Clamp((vx * rx + vy * ry) / maxSpeedCm, -1f, 1f);

        float speed = MathF.Sqrt(vx * vx + vy * vy);
        if (speed < 5f) return;   // standing: keep facing
        float diff = (MathF.Atan2(vy, vx) - MathF.Atan2(fwdY, fwdX)) * (180f / MathF.PI);
        diff %= 360f;
        if (diff > 180f) diff -= 360f; else if (diff <= -180f) diff += 360f;
        float maxStep = MathF.Abs(turnRateDegPerSec) * MathF.Max(dt, 0f);
        yawDelta = Math.Clamp(diff, -maxStep, maxStep);
    }

    // ---- AN_EmitNoise (exec) -------------------------------------------------------------------------
    public static bool EmitNoiseForGraph(float x, float y, float z, float loudnessCm, int tag, int sourceEntity)
        => Native.aver_syn_emit_noise(x, y, z, loudnessCm, tag, sourceEntity) != 0;

    // ---- AN_SetHearing (exec) ------------------------------------------------------------------------
    public static bool SetHearingForGraph(int entity, float sensitivity, float maxRangeCm, float memorySec)
    {
        Native.aver_syn_hearing_attach(entity);
        return Native.aver_syn_hearing_configure(entity, sensitivity, maxRangeCm, memorySec) != 0;
    }

    // ---- AN_GetHeard (pure) --------------------------------------------------------------------------
    /// <summary>"heard" false is ordinary: the listener remembers nothing. Success is false only without a listener component.</summary>
    public static bool GetHeardForGraph(int entity, out bool heard, out float x, out float y, out float z,
                                        out float level, out int tag, out float confidence, out float timeSinceSec)
    {
        heard = Native.aver_syn_hearing_get(entity, out x, out y, out z, out level, out tag, out confidence, out timeSinceSec) != 0;
        if (!heard) { x = y = z = level = confidence = 0f; tag = 0; timeSinceSec = -1f; }
        return true;
    }

    // ---- AN_FindCover (exec) -------------------------------------------------------------------------
    public static bool FindCoverForGraph(int entity, float threatX, float threatY, float threatZ, float maxSeekCm,
                                         float minThreatDistCm, out bool found, out float x, out float y, out int coverId)
    {
        coverId = Native.aver_syn_cover_find(entity, threatX, threatY, threatZ, maxSeekCm, minThreatDistCm, 0, out x, out y);
        found = coverId != 0;
        if (!found) { x = 0f; y = 0f; }
        return true;
    }

    // ---- AN_ReleaseCover (exec) ----------------------------------------------------------------------
    public static bool ReleaseCoverForGraph(int entity) => Native.aver_syn_cover_release(entity) != 0;

    // ---- AN_IsCovered (pure) -------------------------------------------------------------------------
    public static bool IsCoveredForGraph(int entity, float threatX, float threatY, float threatZ)
        => Native.aver_syn_cover_is_covered(entity, threatX, threatY, threatZ) != 0;

    // ---- AN_SquadJoin (exec) -------------------------------------------------------------------------
    public static bool SquadJoinForGraph(int entity, int squadId, float spacingCm)
        => Native.aver_syn_squad_attach(entity, squadId, spacingCm) != 0;

    // ---- AN_SquadSetTarget (exec) --------------------------------------------------------------------
    public static bool SquadSetTargetForGraph(int squadId, float x, float y, float z)
        => Native.aver_syn_squad_set_target(squadId, x, y, z) != 0;

    // ---- AN_GetSquadSlot (pure) ----------------------------------------------------------------------
    /// <summary>"success" is false until the member's squad has a target.</summary>
    public static bool GetSquadSlotForGraph(int entity, out int role, out float x, out float y, out float z)
        => Native.aver_syn_squad_slot(entity, out role, out x, out y, out z) != 0;
}
