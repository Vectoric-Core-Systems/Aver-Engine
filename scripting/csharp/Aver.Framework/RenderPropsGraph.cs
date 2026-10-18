// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the light, camera and particle-emitter Aver Node nodes call (GraphRenderPropsNodeTable.cs; palette
// mirror sandbox/src/GraphNodeDefsRenderProps.hpp). Each is a thin wrapper over a public Entity member in
// EntityRenderProps.cs. A node never throws: a dead entity, a missing component or a missing native library answers
// false/0, logged once per node type.

using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework;

/// <summary>The failure rule shared by the render-props, audio-control, UI-extra and vehicle nodes: false or 0,
/// logged once per node, never thrown. A missing native library is the bare tool process, not a fault, so it stays quiet.</summary>
internal static class FamiliesBLog
{
    private static readonly HashSet<string> Seen = new();

    /// <summary>Logs <paramref name="message"/> the first time <paramref name="key"/> is seen.</summary>
    internal static void Once(string key, string message)
    {
        if (Seen.Add(key)) Log.Warn(message);
    }

    /// <summary>Returns <paramref name="ok"/>; when false, logs <paramref name="why"/> once for the node.</summary>
    internal static bool Ok(string node, bool ok, string why)
    {
        if (!ok) Once(node, $"[{node}] {why}");
        return ok;
    }

    /// <summary>The catch side: returns false after logging an unexpected exception once for the node.</summary>
    internal static bool Failed(string node, Exception e)
    {
        if (e is not (DllNotFoundException or EntryPointNotFoundException))
            Once(node, $"[{node}] {e.GetType().Name}: {e.Message}");
        return false;
    }
}

internal static class RenderPropsGraph
{
    private const string NoLight = "the entity is dead or has no light";
    private const string NoCamera = "the entity is dead or has no camera";
    private const string NoEmitter = "the entity is dead or has no particle emitter";

    /// <summary>AN_SetLightIntensity: the light's lux; 0 turns it off.</summary>
    internal static bool SetLightIntensityForGraph(int entity, float lux)
    {
        try { return FamiliesBLog.Ok("AN_SetLightIntensity", new Entity(entity).SetLightIntensity(lux), NoLight); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetLightIntensity", e); }
    }

    /// <summary>AN_GetLightIntensity: the light's lux; found is false when the entity has no light.</summary>
    internal static bool GetLightIntensityForGraph(int entity, out float lux)
    {
        lux = 0f;
        try { return new Entity(entity).TryGetLightIntensity(out lux); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_GetLightIntensity", e); }
    }

    /// <summary>AN_SetLightColor: linear red, green and blue.</summary>
    internal static bool SetLightColorForGraph(int entity, float r, float g, float b)
    {
        try { return FamiliesBLog.Ok("AN_SetLightColor", new Entity(entity).SetLightColour(new Vec3(r, g, b)), NoLight); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetLightColor", e); }
    }

    /// <summary>AN_SetLightRange: reach in centimetres; 0 leaves it to the renderer.</summary>
    internal static bool SetLightRangeForGraph(int entity, float rangeCm)
    {
        try { return FamiliesBLog.Ok("AN_SetLightRange", new Entity(entity).SetLightRange(rangeCm), NoLight); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetLightRange", e); }
    }

    /// <summary>AN_SetLightCone: a spot light's inner and outer half-angles in degrees.</summary>
    internal static bool SetLightConeForGraph(int entity, float innerDeg, float outerDeg)
    {
        try { return FamiliesBLog.Ok("AN_SetLightCone", new Entity(entity).SetLightCone(innerDeg, outerDeg), NoLight); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetLightCone", e); }
    }

    /// <summary>AN_SetLightShadows: whether the light casts shadows.</summary>
    internal static bool SetLightShadowsForGraph(int entity, bool enabled)
    {
        try { return FamiliesBLog.Ok("AN_SetLightShadows", new Entity(entity).SetLightShadows(enabled), NoLight); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetLightShadows", e); }
    }

    /// <summary>AN_SetCameraFov: vertical field of view in degrees.</summary>
    internal static bool SetCameraFovForGraph(int entity, float fovDeg)
    {
        try { return FamiliesBLog.Ok("AN_SetCameraFov", new Entity(entity).SetCameraFov(fovDeg), NoCamera); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetCameraFov", e); }
    }

    /// <summary>AN_GetCameraFov: vertical field of view in degrees; found is false when the entity has no camera.</summary>
    internal static bool GetCameraFovForGraph(int entity, out float fovDeg)
    {
        fovDeg = 0f;
        try { return new Entity(entity).TryGetCameraFov(out fovDeg); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_GetCameraFov", e); }
    }

    /// <summary>AN_SetCameraClip: near and far clip distances in centimetres (0 &lt; near &lt; far).</summary>
    internal static bool SetCameraClipForGraph(int entity, float nearCm, float farCm)
    {
        try { return FamiliesBLog.Ok("AN_SetCameraClip", new Entity(entity).SetCameraClip(nearCm, farCm), NoCamera + ", or the clip distances are not 0 < near < far"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetCameraClip", e); }
    }

    /// <summary>AN_SetCameraPriority: the highest-priority camera is the view.</summary>
    internal static bool SetCameraPriorityForGraph(int entity, int priority)
    {
        try { return FamiliesBLog.Ok("AN_SetCameraPriority", new Entity(entity).SetCameraPriority(priority), NoCamera); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetCameraPriority", e); }
    }

    /// <summary>AN_SetEmitterEnabled: starts or stops emission.</summary>
    internal static bool SetEmitterEnabledForGraph(int entity, bool enabled)
    {
        try { return FamiliesBLog.Ok("AN_SetEmitterEnabled", new Entity(entity).SetEmitterEnabled(enabled), NoEmitter); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetEmitterEnabled", e); }
    }

    /// <summary>AN_SetEmitterEffect: points the emitter at the effect= content path.</summary>
    internal static bool SetEmitterEffectForGraph(int entity, string effect)
    {
        try { return FamiliesBLog.Ok("AN_SetEmitterEffect", new Entity(entity).SetEmitterEffect(effect), NoEmitter); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetEmitterEffect", e); }
    }

    /// <summary>AN_RestartEmitter: clock to 0 and emission on.</summary>
    internal static bool RestartEmitterForGraph(int entity)
    {
        try { return FamiliesBLog.Ok("AN_RestartEmitter", new Entity(entity).RestartEmitter(), NoEmitter); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_RestartEmitter", e); }
    }

    /// <summary>AN_SpawnEmitterAt: a self-removing emitter entity at a world point; 0 when it could not be made.</summary>
    internal static int SpawnEmitterAtForGraph(float x, float y, float z, float lifetime, string effect)
    {
        try
        {
            Entity spawned = Entity.SpawnEmitterAt(new Vec3(x, y, z), effect, lifetime);
            FamiliesBLog.Ok("AN_SpawnEmitterAt", spawned.IsValid, "no emitter entity could be created (empty effect= or no scene)");
            return spawned.Handle;
        }
        catch (Exception ex)
        {
            FamiliesBLog.Failed("AN_SpawnEmitterAt", ex);
            return 0;
        }
    }
}
