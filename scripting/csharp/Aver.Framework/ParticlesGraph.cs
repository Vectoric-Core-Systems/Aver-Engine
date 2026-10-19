// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The graph-node surface of the Particles family (docs/plans/wave10/GPU_PARTICLES.md section 2.13): the same calls as
// Particles.cs and EntityParticles.cs with the int-entity / primitive-only signatures GraphCompiler's emitted IL needs.
// The rows live in Aver.Graph/GraphParticlesNodeTable.cs; palette mirror: sandbox/src/GraphNodeDefsParticles.hpp.
// Failure rule: false / 0, logged once per node, never thrown. Effect paths and parameter names are NODE attributes.

using Aver.Scene;

namespace Aver.Framework;

internal static class ParticlesGraph
{
    private const string NoProvider = "the host has no particle system, or the entity has no ticked emitter";

    /// <summary>AN_SpawnParticlesAt: plays the effect= once at a point along a direction (zero direction: authored); returns how many particles were accepted.</summary>
    internal static int SpawnParticlesAtForGraph(float x, float y, float z, float dirX, float dirY, float dirZ, int count, string effect)
    {
        try
        {
            int n = Particles.SpawnAt(effect, new Vec3(x, y, z), new Vec3(dirX, dirY, dirZ), count);
            FamiliesBLog.Ok("AN_SpawnParticlesAt", n > 0, "nothing spawned (unknown effect=, count 0, particles off, or the per-frame budget is spent)");
            return n;
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_SpawnParticlesAt", e); return 0; }
    }

    /// <summary>AN_SetParticleFloat: a float user parameter of the entity's effect.</summary>
    internal static bool SetParticleFloatForGraph(int entity, string param, float value)
    {
        try { return FamiliesBLog.Ok("AN_SetParticleFloat", new Entity(entity).SetParticleFloat(param, value), NoProvider); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetParticleFloat", e); }
    }

    /// <summary>AN_SetParticleVector: a vector user parameter.</summary>
    internal static bool SetParticleVectorForGraph(int entity, string param, float x, float y, float z)
    {
        try { return FamiliesBLog.Ok("AN_SetParticleVector", new Entity(entity).SetParticleVector(param, new Vec3(x, y, z)), NoProvider); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetParticleVector", e); }
    }

    /// <summary>AN_SetParticleColor: a colour user parameter.</summary>
    internal static bool SetParticleColorForGraph(int entity, string param, float r, float g, float b, float a)
    {
        try { return FamiliesBLog.Ok("AN_SetParticleColor", new Entity(entity).SetParticleColor(param, r, g, b, a), NoProvider); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetParticleColor", e); }
    }

    /// <summary>AN_BurstEmitter: count extra particles from every emitter of the effect.</summary>
    internal static bool BurstEmitterForGraph(int entity, int count)
    {
        try { return FamiliesBLog.Ok("AN_BurstEmitter", new Entity(entity).BurstEmitter(count), NoProvider); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_BurstEmitter", e); }
    }

    /// <summary>AN_StopEmitter: stops emission, optionally killing the live particles.</summary>
    internal static bool StopEmitterForGraph(int entity, bool killParticles)
    {
        try { return FamiliesBLog.Ok("AN_StopEmitter", new Entity(entity).StopEmitter(killParticles), NoProvider); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_StopEmitter", e); }
    }

    /// <summary>AN_SetEmitterModifiers: scales on rate, new particles' size and speed.</summary>
    internal static bool SetEmitterModifiersForGraph(int entity, float rateScale, float sizeScale, float speedScale)
    {
        try { return FamiliesBLog.Ok("AN_SetEmitterModifiers", new Entity(entity).SetEmitterModifiers(rateScale, sizeScale, speedScale), NoProvider); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetEmitterModifiers", e); }
    }

    /// <summary>AN_GetEmitterState: live particles (a few frames old on the GPU tier) and the tier; found is false for an entity with no ticked emitter.</summary>
    internal static bool GetEmitterStateForGraph(int entity, out int alive, out bool onGpu)
    {
        alive = 0;
        onGpu = false;
        try { return new Entity(entity).TryGetEmitterState(out alive, out onGpu); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_GetEmitterState", e); }
    }

    /// <summary>AN_GetParticleStats: emitters and live particles per tier; ok is false without a particle provider.</summary>
    internal static bool GetParticleStatsForGraph(out int gpuEmitters, out int cpuEmitters, out int gpuParticles, out int cpuParticles)
    {
        gpuEmitters = cpuEmitters = gpuParticles = cpuParticles = 0;
        try
        {
            if (!Particles.TryGetStats(out ParticleStats s)) return false;
            gpuEmitters = s.GpuEmitters;
            cpuEmitters = s.CpuEmitters;
            gpuParticles = s.GpuParticles;
            cpuParticles = s.CpuParticles;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_GetParticleStats", e); }
    }
}
