// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Local fog volumes and reflection probes a script or a converted level places. Design: docs/plans/fabric/G4_RENDER.md R8.

namespace Aver.Framework;

/// <summary>Godot's <c>RenderingServer.FogVolumeShape</c> numbers, the same as <c>AVER_FW_FOGVOL_*</c>.</summary>
public enum FogVolumeShapeKind
{
    Ellipsoid = 0,
    Cone = 1,
    Cylinder = 2,
    Box = 3,
    /// <summary>No shape: the fog fills the whole volumetric-fog volume.</summary>
    World = 4,
}

/// <summary>The reflection probes that light a point.</summary>
public readonly struct ProbeBlend
{
    /// <summary>Up to four probe ids, the most specific (smallest) first.</summary>
    public int[] Ids { get; init; }
    /// <summary>Each probe's share of the blend (its intensity included).</summary>
    public float[] Weights { get; init; }
    /// <summary>What is left for the sky / default environment, 0 to 1.</summary>
    public float Rest { get; init; }
}

/// <summary>
/// Local fog volumes and reflection probes (Godot's FogVolume and ReflectionProbe). Units are the engine's: centimetres, +Z up, linear colours. A matrix is the
/// engine's 12-float instance matrix (three basis rows then the translation, row vectors); half extents are along the engine's local axes before the matrix's scale.
/// </summary>
/// <remarks>
/// <para>FOG VOLUMES add extinction, scattering and emission inside the renderer's volumetric fog (<c>RENDER.VOLFOG 1</c>); with it off they are recorded and draw
/// nothing. At most 16 are drawn in a frame, the nearest the camera. REFLECTION PROBES are recorded and answer <see cref="SelectProbes"/> and
/// <see cref="ProjectProbeDirection"/>; the renderer does not sample a captured environment yet (docs/plans/fabric/G4_RENDER.md R8). Calls return false instead of
/// throwing, and without a host they do nothing. A key is stable across loads; creating one that exists returns its id.</para>
/// </remarks>
public static class EnvFx
{
    public const int MaxFogVolumes = 64;
    public const int MaxProbes = 32;

    // ---- fog volumes ----

    public static int CreateFogVolume(string key) => string.IsNullOrEmpty(key) ? 0 : FwEnvFx.aver_fw_fogvol_create(key);
    public static bool DestroyFogVolume(int id) => id > 0 && FwEnvFx.aver_fw_fogvol_destroy(id) != 0;
    public static int FindFogVolume(string key) => string.IsNullOrEmpty(key) ? 0 : FwEnvFx.aver_fw_fogvol_find(key);

    /// <summary>Everything about the volume in one call. Density is per metre (Godot's FogMaterial.density; negative carves), height falloff per metre, edge fade a
    /// fraction of the half size. False for an unknown id, a non-finite value or a matrix that cannot be inverted.</summary>
    public static bool SetFogVolume(int id, FogVolumeShapeKind shape, float[] world12, float[] halfExtentCm3, float densityPerMetre, float[]? albedo3 = null,
                                    float[]? emission3 = null, float heightFalloff = 0f, float edgeFade = 0.1f) =>
        id > 0 && world12.Length >= 12 && halfExtentCm3.Length >= 3 &&
        FwEnvFx.aver_fw_fogvol_set(id, (int)shape, world12, halfExtentCm3, densityPerMetre, albedo3, emission3, heightFalloff, edgeFade) != 0;

    public static bool SetFogVolumeEnabled(int id, bool on) => id > 0 && FwEnvFx.aver_fw_fogvol_set_enabled(id, on ? 1 : 0) != 0;

    public static int FogVolumeCount => FwEnvFx.aver_fw_fogvol_count();

    /// <summary>The summed signed extinction per centimetre of every enabled volume at an engine-space point.</summary>
    public static float FogDensityAt(float x, float y, float z) => FwEnvFx.aver_fw_fogvol_density_at(x, y, z);

    // ---- reflection probes ----

    public static int CreateProbe(string key) => string.IsNullOrEmpty(key) ? 0 : FwEnvFx.aver_fw_probe_create(key);
    public static bool DestroyProbe(int id) => id > 0 && FwEnvFx.aver_fw_probe_destroy(id) != 0;
    public static int FindProbe(string key) => string.IsNullOrEmpty(key) ? 0 : FwEnvFx.aver_fw_probe_find(key);

    public static bool SetProbeBox(int id, float[] world12, float[] halfExtentCm3, float[]? originOffsetCm3 = null) =>
        id > 0 && world12.Length >= 12 && halfExtentCm3.Length >= 3 && FwEnvFx.aver_fw_probe_set_box(id, world12, halfExtentCm3, originOffsetCm3) != 0;

    public static bool SetProbeLook(int id, float intensity, float blendDistanceCm, float maxDistanceCm, bool boxProjection, bool interior, bool updateAlways,
                                    int resolution) =>
        id > 0 && FwEnvFx.aver_fw_probe_set_look(id, intensity, blendDistanceCm, maxDistanceCm, boxProjection ? 1 : 0, interior ? 1 : 0, updateAlways ? 1 : 0, resolution) != 0;

    /// <summary>Mode 0 disabled, 1 environment, 2 colour (Godot's ReflectionProbe.AmbientMode).</summary>
    public static bool SetProbeAmbient(int id, int mode, float[]? colour3, float energy) => id > 0 && FwEnvFx.aver_fw_probe_set_ambient(id, mode, colour3, energy) != 0;

    public static bool SetProbeMasks(int id, uint cullMask, uint reflectionMask) => id > 0 && FwEnvFx.aver_fw_probe_set_masks(id, cullMask, reflectionMask) != 0;
    public static bool SetProbeEnabled(int id, bool on) => id > 0 && FwEnvFx.aver_fw_probe_set_enabled(id, on ? 1 : 0) != 0;
    public static int ProbeCount => FwEnvFx.aver_fw_probe_count();

    /// <summary>The probes that light an engine-space point seen from a camera position, with their shares of the blend.</summary>
    public static ProbeBlend SelectProbes(float x, float y, float z, float cameraX, float cameraY, float cameraZ)
    {
        var ids = new int[4];
        var weights = new float[4];
        int n = FwEnvFx.aver_fw_probe_select(x, y, z, cameraX, cameraY, cameraZ, ids, weights, out float rest);
        return new ProbeBlend { Ids = ids.AsSpan(0, n).ToArray(), Weights = weights.AsSpan(0, n).ToArray(), Rest = rest };
    }

    /// <summary>The direction to look a probe's capture up with for a reflection ray from a point (box projection applied); the input direction for an unknown probe.</summary>
    public static (float X, float Y, float Z) ProjectProbeDirection(int id, float x, float y, float z, float dx, float dy, float dz)
    {
        var o = new float[3];
        FwEnvFx.aver_fw_probe_project(id, x, y, z, dx, dy, dz, o);
        return (o[0], o[1], o[2]);
    }
}
