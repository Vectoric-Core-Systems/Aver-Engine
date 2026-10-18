// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Lights, cameras and particle emitters from a script: the typed face of CLight, CCamera and CParticleEmitter.
// Every member writes through the component field ABI (GetField/SetField), so it works on any entity that carries the
// component and needs no native entry point of its own. The Aver Node nodes in RenderPropsGraph.cs wrap these.
// Design: docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2 (RenderProps).

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private const int LightNoShadowsBit = 0x1;      // kLightNoShadows (Components.hpp)
    private const int EmitterStoppedBit = 0x1;      // kParticleEmitterStopped; negative sense, so zeroed storage emits
    private const float DegToRad = MathF.PI / 180f;
    private const float RadToDeg = 180f / MathF.PI;
    private const float MaxConeDegrees = 89.5f;

    // ---- Lights -----------------------------------------------------------------------------------------

    /// <summary>True when this entity carries a light.</summary>
    public bool HasLight => HasComponent(Component.Light);

    /// <summary>Sets the light's intensity in lux (candela for a point or spot light). Negative clamps to 0, which is
    /// how a light is turned off: a light has no enable bit. False for a dead entity, no light, or a non-finite value.</summary>
    public bool SetLightIntensity(float lux) =>
        float.IsFinite(lux) && HasComponent(Component.Light) && SetFloat("CLight.intensityLux", MathF.Max(lux, 0f));

    /// <summary>Reads the light's intensity in lux. False (and 0) when the entity has no light.</summary>
    public bool TryGetLightIntensity(out float lux)
    {
        lux = 0f;
        if (!HasComponent(Component.Light)) return false;
        lux = GetFloat("CLight.intensityLux");
        return true;
    }

    /// <summary>Sets the light's linear colour. Components clamp to 0 or more.</summary>
    public bool SetLightColour(Vec3 rgb) =>
        float.IsFinite(rgb.X) && float.IsFinite(rgb.Y) && float.IsFinite(rgb.Z) && HasComponent(Component.Light) &&
        SetVec3("CLight.colour", new Vec3(MathF.Max(rgb.X, 0f), MathF.Max(rgb.Y, 0f), MathF.Max(rgb.Z, 0f)));

    /// <summary>Sets the light's reach in centimetres; 0 leaves it to the renderer.</summary>
    public bool SetLightRange(float rangeCm) =>
        float.IsFinite(rangeCm) && HasComponent(Component.Light) && SetFloat("CLight.rangeCm", MathF.Max(rangeCm, 0f));

    /// <summary>Turns a spot light's cone angles (half-angles, degrees) into the cosines the light stores. The outer
    /// angle clamps to 0..89.5 and the inner to 0..outer, so the inner cone never exceeds the outer.</summary>
    public static void LightConeCosines(float innerDegrees, float outerDegrees, out float innerCos, out float outerCos)
    {
        float outer = Math.Clamp(float.IsFinite(outerDegrees) ? outerDegrees : 0f, 0f, MaxConeDegrees);
        float inner = Math.Clamp(float.IsFinite(innerDegrees) ? innerDegrees : 0f, 0f, outer);
        innerCos = MathF.Cos(inner * DegToRad);
        outerCos = MathF.Cos(outer * DegToRad);
    }

    /// <summary>Sets a spot light's cone (half-angles in degrees; see <see cref="LightConeCosines"/>).</summary>
    public bool SetLightCone(float innerDegrees, float outerDegrees)
    {
        if (!HasComponent(Component.Light)) return false;
        LightConeCosines(innerDegrees, outerDegrees, out float innerCos, out float outerCos);
        return SetFloat("CLight.innerCos", innerCos) & SetFloat("CLight.outerCos", outerCos);
    }

    /// <summary>Whether the light casts shadows. The component stores the negative flag, so zeroed storage casts.</summary>
    public bool SetLightShadows(bool enabled)
    {
        if (!HasComponent(Component.Light)) return false;
        int flags = GetInt("CLight.flags");
        return SetInt("CLight.flags", enabled ? flags & ~LightNoShadowsBit : flags | LightNoShadowsBit);
    }

    // ---- Cameras ----------------------------------------------------------------------------------------

    /// <summary>True when this entity carries a camera.</summary>
    public bool HasCamera => HasComponent(Component.Camera);

    /// <summary>Sets the vertical field of view in degrees, clamped to 1..170 (the component stores radians).</summary>
    public bool SetCameraFov(float fovDegrees) =>
        float.IsFinite(fovDegrees) && HasComponent(Component.Camera) &&
        SetFloat("CCamera.fovYRad", Math.Clamp(fovDegrees, 1f, 170f) * DegToRad);

    /// <summary>Reads the vertical field of view in degrees. False (and 0) when the entity has no camera.</summary>
    public bool TryGetCameraFov(out float fovDegrees)
    {
        fovDegrees = 0f;
        if (!HasComponent(Component.Camera)) return false;
        fovDegrees = GetFloat("CCamera.fovYRad") * RadToDeg;
        return true;
    }

    /// <summary>Sets the near and far clip distances in centimetres. False unless 0 &lt; near &lt; far.</summary>
    public bool SetCameraClip(float nearCm, float farCm) =>
        float.IsFinite(nearCm) && float.IsFinite(farCm) && nearCm > 0f && farCm > nearCm && HasComponent(Component.Camera) &&
        (SetFloat("CCamera.nearCm", nearCm) & SetFloat("CCamera.farCm", farCm));

    /// <summary>Sets the camera's priority; the highest-priority camera is the one the view follows.</summary>
    public bool SetCameraPriority(int priority) => HasComponent(Component.Camera) && SetInt("CCamera.priority", priority);

    // ---- Particle emitters ------------------------------------------------------------------------------

    /// <summary>True when this entity carries a particle emitter.</summary>
    public bool HasEmitter => HasComponent(Component.ParticleEmitter);

    /// <summary>Starts or stops emission. Starting an emitter that was stopped also restarts its clock.</summary>
    public bool SetEmitterEnabled(bool enabled)
    {
        if (!HasComponent(Component.ParticleEmitter)) return false;
        int flags = GetInt("CParticleEmitter.flags");
        bool wasStopped = (flags & EmitterStoppedBit) != 0;
        if (enabled && wasStopped) SetFloat("CParticleEmitter.age", 0f);
        return SetInt("CParticleEmitter.flags", enabled ? flags & ~EmitterStoppedBit : flags | EmitterStoppedBit);
    }

    /// <summary>Points the emitter at a particle effect (.ocparticle) by its content path.</summary>
    public bool SetEmitterEffect(string effectPath) =>
        !string.IsNullOrEmpty(effectPath) && HasComponent(Component.ParticleEmitter) &&
        SetInt64("CParticleEmitter.effect", Assets.ObjectIdOf(effectPath));

    /// <summary>Restarts the emitter: clock back to 0 and emission on.</summary>
    public bool RestartEmitter()
    {
        if (!HasComponent(Component.ParticleEmitter)) return false;
        SetFloat("CParticleEmitter.age", 0f);
        return SetInt("CParticleEmitter.flags", GetInt("CParticleEmitter.flags") & ~EmitterStoppedBit);
    }

    /// <summary>Creates an entity at <paramref name="position"/> that plays <paramref name="effectPath"/>, and destroys
    /// it after <paramref name="lifetimeSeconds"/> (0 or less: never). Returns <see cref="None"/> when the entity or its
    /// emitter could not be made.</summary>
    public static Entity SpawnEmitterAt(Vec3 position, string effectPath, float lifetimeSeconds = 3f)
    {
        if (string.IsNullOrEmpty(effectPath)) return None;
        Entity e = Create("Emitter");
        if (!e.IsValid) return None;
        if (!e.AddComponent(Component.ParticleEmitter))
        {
            e.Destroy();
            return None;
        }
        e.SetLocalPosition(position);
        e.SetInt64("CParticleEmitter.effect", Assets.ObjectIdOf(effectPath));
        if (lifetimeSeconds > 0f && float.IsFinite(lifetimeSeconds))
        {
            int handle = e.Handle;
            Timers.Set(lifetimeSeconds, () =>
            {
                var gone = new Entity(handle);
                if (gone.IsAlive) gone.Destroy();
            }, owner: handle);
        }
        return e;
    }
}
