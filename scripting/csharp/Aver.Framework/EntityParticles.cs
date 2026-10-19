// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// An entity's particle emitter from a script: user parameters an effect declares (wind strength, an attractor point, a
// tint), bursts, stop, rate/size/speed scales and a state read. Design: docs/plans/wave10/GPU_PARTICLES.md section 2.11.
//
// Relayed through aver_fw_particles_*: every call is queued and applied at the top of the next particle tick, in call
// order; a host without a particle provider answers false. Emitter on/off and effect choice stay in EntityRenderProps.cs.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private static long ParticleParamHash(string name) => Assets.ObjectIdOf(name);

    /// <summary>Sets a float user parameter the entity's effect declares with <c>PARAM name float</c>. False for an empty
    /// name or a host without a particle provider; a name the effect does not declare is silently ignored.</summary>
    public bool SetParticleFloat(string name, float value) =>
        !string.IsNullOrEmpty(name) &&
        ParticleCall(Handle, h => FwParticles.aver_fw_particles_set_param(h, ParticleParamHash(name), (int)ParticleParamKind.Float, value, 0f, 0f, 0f));

    /// <summary>Sets a vector user parameter (<c>PARAM name vec3</c>): a wind direction, an attractor position.</summary>
    public bool SetParticleVector(string name, Vec3 value) =>
        !string.IsNullOrEmpty(name) &&
        ParticleCall(Handle, h => FwParticles.aver_fw_particles_set_param(h, ParticleParamHash(name), (int)ParticleParamKind.Vector, value.X, value.Y, value.Z, 0f));

    /// <summary>Sets a colour user parameter (<c>PARAM name color</c>).</summary>
    public bool SetParticleColor(string name, float r, float g, float b, float a = 1f) =>
        !string.IsNullOrEmpty(name) &&
        ParticleCall(Handle, h => FwParticles.aver_fw_particles_set_param(h, ParticleParamHash(name), (int)ParticleParamKind.Color, r, g, b, a));

    /// <summary>Spawns <paramref name="count"/> extra particles from every emitter of the entity's effect, once.</summary>
    public bool BurstEmitter(int count) =>
        count > 0 && ParticleCall(Handle, h => FwParticles.aver_fw_particles_burst(h, count));

    /// <summary>Stops emission (like SetEmitterEnabled(false)); with <paramref name="killParticles"/> the live particles
    /// go too, otherwise they finish their lives.</summary>
    public bool StopEmitter(bool killParticles = false) =>
        ParticleCall(Handle, h => FwParticles.aver_fw_particles_stop(h, killParticles ? 1 : 0));

    /// <summary>Scales the emitter's rate, new particles' size and initial speed (1 = authored). Applies to particles
    /// spawned from now on.</summary>
    public bool SetEmitterModifiers(float rateScale = 1f, float sizeScale = 1f, float speedScale = 1f) =>
        float.IsFinite(rateScale) && float.IsFinite(sizeScale) && float.IsFinite(speedScale) &&
        ParticleCall(Handle, h => FwParticles.aver_fw_particles_set_modifiers(h, rateScale, sizeScale, speedScale));

    /// <summary>Reads how many particles the entity's emitters hold and which tier simulates them. On the GPU tier the
    /// count is up to three frames old (it never stalls the GPU). False when the entity has no ticked emitter or the
    /// host has no particles.</summary>
    public bool TryGetEmitterState(out int aliveParticles, out bool onGpu)
    {
        aliveParticles = 0;
        onGpu = false;
        try
        {
            if (FwParticles.aver_fw_particles_emitter_state(Handle, out int alive, out int tier) == 0) return false;
            aliveParticles = alive;
            onGpu = tier != 0;
            return true;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    // Structs cannot capture `this` in a lambda: the handle travels as an argument.
    private static bool ParticleCall(int handle, Func<int, int> call) => ParticleCall(() => call(handle));

    private static bool ParticleCall(Func<int> call)
    {
        try { return call() != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }
}
