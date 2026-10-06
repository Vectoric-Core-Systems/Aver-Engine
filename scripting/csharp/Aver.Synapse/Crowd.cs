// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
using System.Runtime.InteropServices;

namespace Aver.Synapse;

/// <summary>Which solver plans crowd avoidance.</summary>
public enum CrowdBackend
{
    /// <summary>Reciprocal avoidance over a spatial hash on the CPU. Deterministic. The default; good for hundreds of agents.</summary>
    Cpu = 0,
    /// <summary>A compute shader, for thousands. Needs the host to install one; falls back to <see cref="Cpu"/> without it. Plans from a few frames ago.</summary>
    Gpu = 1,
}

/// <summary>How a crowd agent chooses where it wants to go.</summary>
public enum CrowdMode
{
    /// <summary>Head for the CSynapseAgent path target while it is pathing.</summary>
    FollowAgent = 0,
    Seek = 1,
    Arrive = 2,
    Flee = 3,
    Wander = 4,
    /// <summary>Stand still; others walk around it.</summary>
    Hold = 5,
}

/// <summary>Who moves the entities.</summary>
public enum CrowdDrive
{
    /// <summary>The crowd only publishes a velocity; a character controller moves the entity.</summary>
    Advise = 0,
    /// <summary>The crowd writes the entity's position itself. For large crowds without physics capsules.</summary>
    Move = 1,
}

/// <summary>Steering, avoidance and crowd settings. Units are centimetres and seconds, +X forward, +Y right.</summary>
public static class Crowd
{
    /// <summary>Makes the entity a crowd agent, with radius and speed taken from its path agent when it has one.</summary>
    public static bool Attach(int entity) => Native.aver_syn_crowd_attach(entity) != 0;

    public static bool Configure(int entity, float radiusCm, float maxSpeedCm, float maxAccelCm = 1200f, float priority = 0f)
        => Native.aver_syn_crowd_configure(entity, radiusCm, maxSpeedCm, maxAccelCm, priority) != 0;

    /// <summary>Chooses the steering behaviour. <paramref name="x"/>/<paramref name="y"/>/<paramref name="z"/> is the target (Seek, Arrive) or the threat (Flee).</summary>
    public static bool SetMode(int entity, CrowdMode mode, float x = 0f, float y = 0f, float z = 0f)
        => Native.aver_syn_crowd_set_mode(entity, (int)mode, x, y, z) != 0;

    public static bool SetEnabled(int entity, bool enabled) => Native.aver_syn_crowd_set_enabled(entity, enabled ? 1 : 0) != 0;

    /// <summary>The collision-free velocity for this tick. False when the entity is not a simulated agent.</summary>
    public static bool TryGetVelocity(int entity, out float vx, out float vy, out float speed)
        => Native.aver_syn_crowd_velocity(entity, out vx, out vy, out speed) != 0;

    /// <summary>The solver you asked for. Setting <see cref="CrowdBackend.Gpu"/> without an installed GPU solver keeps planning on the CPU.</summary>
    public static CrowdBackend Backend
    {
        get => (CrowdBackend)Native.aver_syn_crowd_backend();
        set => Native.aver_syn_crowd_set_backend((int)value);
    }

    /// <summary>The solver actually in use right now ("cpu-orca" or "gpu-orca").</summary>
    public static string ActiveBackendName => Marshal.PtrToStringUTF8(Native.aver_syn_crowd_backend_name()) ?? "";

    /// <summary>The most agents simulated; the rest (highest entity ids) are left alone.</summary>
    public static int MaxAgents
    {
        get => Native.aver_syn_crowd_max_agents();
        set => Native.aver_syn_crowd_set_max_agents(value);
    }

    /// <summary>Agents simulated on the last tick.</summary>
    public static int AgentCount => Native.aver_syn_crowd_agent_count();

    /// <summary>Agents left out of the last tick because the cap was reached.</summary>
    public static int OverflowCount => Native.aver_syn_crowd_overflow_count();

    public static CrowdDrive Drive
    {
        set => Native.aver_syn_crowd_set_drive((int)value);
    }
}
