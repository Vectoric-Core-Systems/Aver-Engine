// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The reflection targets for the AN_ crowd, hearing, cover and squad nodes, kept out of
// GraphInterop (a non-partial class other work edits) in a class of their own. Compiled only when
// AVER_SYNAPSE_AI is defined, which Aver.Framework.csproj does together with the project reference
// to Aver.Synapse -- see docs/AI_CROWDS_HEARING_COVER.md for the two lines.
//
// GraphCompiler looks each method up by name with GraphInteropSynapseAi.Method(name), in the same
// shape as its other InteropMethod fields. Every signature is: the node's input pins in order, then
// its output pins in order as out parameters, "success" being the return value.
#if AVER_SYNAPSE_AI
using System.Reflection;
using Aver.Scene;
using S = Aver.Synapse.SynapseAiGraph;

namespace Aver.Framework;

internal static class GraphInteropSynapseAi
{
    /// <summary>The method GraphCompiler emits a call to for node <paramref name="name"/> (e.g. "CrowdSetAgentForGraph").</summary>
    internal static MethodInfo Method(string name)
        => typeof(GraphInteropSynapseAi).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static)
           ?? throw new InvalidOperationException($"Aver.Framework.GraphInteropSynapseAi.{name} was not found by reflection");

    // ---- exec nodes ------------------------------------------------------------------------------
    internal static bool CrowdSetAgentForGraph(int entity, float radiusCm, float maxSpeedCm, float maxAccelCm, float priority)
        => S.CrowdSetAgentForGraph(entity, radiusCm, maxSpeedCm, maxAccelCm, priority);

    internal static bool CrowdSetModeForGraph(int entity, int mode, float x, float y, float z)
        => S.CrowdSetModeForGraph(entity, mode, x, y, z);

    internal static bool CrowdSetBackendForGraph(int backend, int maxAgents)
        => S.CrowdSetBackendForGraph(backend, maxAgents);

    internal static bool EmitNoiseForGraph(float x, float y, float z, float loudnessCm, int tag, int source)
        => S.EmitNoiseForGraph(x, y, z, loudnessCm, tag, source);

    internal static bool SetHearingForGraph(int entity, float sensitivity, float maxRangeCm, float memorySec)
        => S.SetHearingForGraph(entity, sensitivity, maxRangeCm, memorySec);

    internal static bool FindCoverForGraph(int entity, float threatX, float threatY, float threatZ, float maxSeekCm,
                                           float minThreatDistCm, out bool found, out float x, out float y, out int coverId)
        => S.FindCoverForGraph(entity, threatX, threatY, threatZ, maxSeekCm, minThreatDistCm, out found, out x, out y, out coverId);

    internal static bool ReleaseCoverForGraph(int entity) => S.ReleaseCoverForGraph(entity);

    internal static bool SquadJoinForGraph(int entity, int squadId, float spacingCm)
        => S.SquadJoinForGraph(entity, squadId, spacingCm);

    internal static bool SquadSetTargetForGraph(int squadId, float x, float y, float z)
        => S.SquadSetTargetForGraph(squadId, x, y, z);

    // ---- pure nodes ------------------------------------------------------------------------------
    internal static bool GetCrowdVelocityForGraph(int entity, out float vx, out float vy, out float speed)
        => S.GetCrowdVelocityForGraph(entity, out vx, out vy, out speed);

    /// <summary>AN_CrowdSteer: the crowd velocity as CharacterMove input. "success" is false for a dead
    /// entity; an entity that is not a simulated agent reads as standing still (all zeros, success).</summary>
    internal static bool CrowdSteerForGraph(int entity, float dt, float turnRateDegPerSec, float maxSpeedCm,
                                            out float forward, out float right, out float yawDelta)
    {
        forward = 0f; right = 0f; yawDelta = 0f;
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        if (!S.GetCrowdVelocityForGraph(entity, out float vx, out float vy, out _)) return true;
        Vec3 fwd = e.WorldForward;
        S.SteerFromVelocity(vx, vy, fwd.X, fwd.Y, maxSpeedCm, dt, turnRateDegPerSec, out forward, out right, out yawDelta);
        return true;
    }

    internal static bool GetHeardForGraph(int entity, out bool heard, out float x, out float y, out float z,
                                          out float level, out int tag, out float confidence, out float timeSinceSec)
        => S.GetHeardForGraph(entity, out heard, out x, out y, out z, out level, out tag, out confidence, out timeSinceSec);

    /// <summary>AN_IsCovered has a single "covered" output, so the return value is that output.</summary>
    internal static bool IsCoveredForGraph(int entity, float threatX, float threatY, float threatZ)
        => S.IsCoveredForGraph(entity, threatX, threatY, threatZ);

    internal static bool GetSquadSlotForGraph(int entity, out int role, out float x, out float y, out float z)
        => S.GetSquadSlotForGraph(entity, out role, out x, out y, out z);
}
#endif
