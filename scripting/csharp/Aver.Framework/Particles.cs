// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Particles from a script: play an effect once at a point (no entity), and read the particle system's counters.
// Design: docs/plans/wave10/GPU_PARTICLES.md section 2.10 and 2.11; guide: docs/PARTICLES.md.
//
// RELAYED through the framework like montages (aver_fw_particles_*): the framework links no particle module, so a host
// that installed no provider answers 0 / false from every call here rather than throwing. An effect is addressed by its
// content-relative path (hashed with the same fnv1a64 the editor registers effects under), so a script keeps paths in
// its data and converts once.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The particle system's counters, as <see cref="Particles.TryGetStats"/> returns them. GPU particle counts are
/// up to three frames old (no readback stalls).</summary>
/// <param name="GpuEmitters">Emitter instances simulating on the GPU.</param>
/// <param name="CpuEmitters">Emitter instances simulating on the CPU.</param>
/// <param name="GpuParticles">Live particles on the GPU tier (approximate).</param>
/// <param name="CpuParticles">Live particles on the CPU tier.</param>
/// <param name="SpawnAtDropped">Spawn-at particles refused by the per-frame budget since the system started.</param>
/// <param name="GpuAllocatedKB">GPU memory the particle pool holds, in kilobytes.</param>
public readonly record struct ParticleStats(int GpuEmitters, int CpuEmitters, int GpuParticles, int CpuParticles,
                                            int SpawnAtDropped, int GpuAllocatedKB);

/// <summary>The kind of a particle user parameter, as an effect declares it with PARAM.</summary>
public enum ParticleParamKind
{
    /// <summary>One number (<c>PARAM name float v</c>).</summary>
    Float = 0,
    /// <summary>Three numbers: a position, direction or force (<c>PARAM name vec3 x y z</c>).</summary>
    Vector = 1,
    /// <summary>Red, green, blue and alpha (<c>PARAM name color r g b a</c>).</summary>
    Color = 2,
}

/// <summary>Fire-and-forget particle effects and the particle system's budgets.</summary>
public static class Particles
{
    private static long IdOf(string effect) => Assets.ObjectIdOf(effect.Replace('\\', '/'));

    /// <summary>Spawns <paramref name="count"/> particles of the effect (a content path ending .ocparticle) at a world
    /// point, fired along <paramref name="direction"/> (effect +Z maps onto it; zero keeps the effect's authored
    /// direction). No entity is created: a thousand skid puffs are one shared simulation that retires after five idle
    /// seconds. Applied at the start of the next particle tick, so a call made in this frame's gameplay step shows this
    /// frame. Returns how many particles were accepted after the effect's own cap and the per-frame budget; 0 for an
    /// unknown effect or a host without particles.</summary>
    public static int SpawnAt(string effect, Vec3 position, Vec3 direction, int count = 1)
    {
        if (string.IsNullOrEmpty(effect) || count <= 0) return 0;
        try { return FwParticles.aver_fw_particles_spawn_at(IdOf(effect), position.X, position.Y, position.Z, direction.X, direction.Y, direction.Z, count); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    /// <summary>As above with speed, size and lifetime scales and an inherited velocity (a vehicle's own velocity, for
    /// sparks and smoke: effects that carry the spawn.inheritvelocity module add it, scaled, to each particle).</summary>
    public static int SpawnAt(string effect, Vec3 position, Vec3 direction, int count, float speedScale,
                              float sizeScale = 1f, float lifetimeScale = 1f, Vec3 inheritedVelocity = default)
    {
        if (string.IsNullOrEmpty(effect) || count <= 0) return 0;
        var a = new float[16]
        {
            position.X, position.Y, position.Z, direction.X, direction.Y, direction.Z,
            inheritedVelocity.X, inheritedVelocity.Y, inheritedVelocity.Z, speedScale, sizeScale, lifetimeScale, 1f, 1f, 1f, 1f,
        };
        try { return FwParticles.aver_fw_particles_spawn_at_ex(IdOf(effect), a, count); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    /// <summary>The same by effect id (<c>Assets.ObjectIdOf(path)</c>), for callers that hash once.</summary>
    public static int SpawnAt(long effectId, Vec3 position, Vec3 direction, int count = 1)
    {
        if (effectId == 0 || count <= 0) return 0;
        try { return FwParticles.aver_fw_particles_spawn_at(effectId, position.X, position.Y, position.Z, direction.X, direction.Y, direction.Z, count); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    /// <summary>Sets the particle budgets: live GPU particles (allocated capacity), live CPU particles, and spawn-at
    /// particles per frame. A negative value keeps the current one. False without a particle provider.</summary>
    public static bool SetBudget(int maxGpuParticles = 2_000_000, int maxCpuParticles = 200_000, int maxSpawnAtPerFrame = 2048)
    {
        try { return FwParticles.aver_fw_particles_set_budget(maxGpuParticles, maxCpuParticles, maxSpawnAtPerFrame) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Chooses where emitters may simulate: 0 everything on the CPU, 1 automatic (big emitters on the GPU),
    /// 2 every emitter that can on the GPU. Applies to emitters created afterwards. False without a provider.</summary>
    public static bool SetGpuMode(int mode)
    {
        try { return FwParticles.aver_fw_particles_set_gpu_mode(mode) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Reads the particle system's counters. False (and zeros) when the host has no particles.</summary>
    public static bool TryGetStats(out ParticleStats stats)
    {
        stats = default;
        try
        {
            var s = new int[6];
            if (FwParticles.aver_fw_particles_stats(s) == 0) return false;
            stats = new ParticleStats(s[0], s[1], s[2], s[3], s[4], s[5]);
            return true;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }
}
