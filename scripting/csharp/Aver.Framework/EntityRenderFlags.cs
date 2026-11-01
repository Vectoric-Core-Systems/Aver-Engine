// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Per-instance render flags of a mesh (P5, F32): shadow mode, GI mode, cull margin, occlusion opt-out and LOD bias. They live in CMeshRenderer
// (renderFlags, cullMarginCm, lodBias) and are written through the component field ABI like the rest of Entity's render properties.

namespace Aver.Framework;

/// <summary>How a mesh takes part in the sun's shadows (<see cref="Entity.SetShadowMode"/>).</summary>
public enum MeshShadowMode
{
    /// <summary>Casts shadows (the default).</summary>
    On = 0,
    /// <summary>Casts none into the cascade and GI shadow maps. The ray-traced sun shadow still sees it.</summary>
    Off = 1,
    /// <summary>Invisible to every camera but kept for shadows, GI and ray tracing.</summary>
    ShadowsOnly = 2,
}

/// <summary>How a mesh takes part in the voxel GI (<see cref="Entity.SetGiMode"/>).</summary>
public enum MeshGiMode
{
    /// <summary>Part of the voxel bake (the default).</summary>
    Static = 0,
    /// <summary>Kept out of the bake, as a mover is.</summary>
    Dynamic = 1,
    /// <summary>Kept out of the bake, as a mover is.</summary>
    Disabled = 2,
}

public readonly partial struct Entity
{
    private const int RenderShadowMask = 0x3, RenderGiShift = 2, RenderGiMask = 0x3 << RenderGiShift, RenderIgnoreOcclusionBit = 0x10;   // kRender* in Components.hpp

    /// <summary>Sets whether the mesh casts shadows, or only shadows. False for a dead entity or one without a mesh.</summary>
    public bool SetShadowMode(MeshShadowMode mode)
    {
        if (!HasComponent(Component.MeshRenderer)) return false;
        return SetInt("CMeshRenderer.renderFlags", (GetInt("CMeshRenderer.renderFlags") & ~RenderShadowMask) | ((int)mode & RenderShadowMask));
    }

    /// <summary>The mesh's shadow mode (<see cref="MeshShadowMode.On"/> when it has no mesh).</summary>
    public MeshShadowMode GetShadowMode() =>
        HasComponent(Component.MeshRenderer) ? (MeshShadowMode)(GetInt("CMeshRenderer.renderFlags") & RenderShadowMask) : MeshShadowMode.On;

    /// <summary>Sets how the mesh takes part in the voxel GI.</summary>
    public bool SetGiMode(MeshGiMode mode)
    {
        if (!HasComponent(Component.MeshRenderer)) return false;
        return SetInt("CMeshRenderer.renderFlags", (GetInt("CMeshRenderer.renderFlags") & ~RenderGiMask) | (((int)mode << RenderGiShift) & RenderGiMask));
    }

    /// <summary>The mesh's GI mode (<see cref="MeshGiMode.Static"/> when it has no mesh).</summary>
    public MeshGiMode GetGiMode() =>
        HasComponent(Component.MeshRenderer) ? (MeshGiMode)((GetInt("CMeshRenderer.renderFlags") & RenderGiMask) >> RenderGiShift) : MeshGiMode.Static;

    /// <summary>Grows the box the frustum and occlusion tests use by this many centimetres, so a mesh whose vertices a shader moves is not culled early. Negative clamps to 0.</summary>
    public bool SetCullMargin(float marginCm) =>
        float.IsFinite(marginCm) && HasComponent(Component.MeshRenderer) && SetFloat("CMeshRenderer.cullMarginCm", MathF.Max(marginCm, 0f));

    /// <summary>The cull margin in centimetres (0 when the entity has no mesh).</summary>
    public float GetCullMargin() => HasComponent(Component.MeshRenderer) ? GetFloat("CMeshRenderer.cullMarginCm") : 0f;

    /// <summary>Opts the mesh out of the occlusion test (editor Play; the shipped runtime has no occlusion culling).</summary>
    public bool SetIgnoreOcclusion(bool ignore)
    {
        if (!HasComponent(Component.MeshRenderer)) return false;
        int flags = GetInt("CMeshRenderer.renderFlags");
        return SetInt("CMeshRenderer.renderFlags", ignore ? flags | RenderIgnoreOcclusionBit : flags & ~RenderIgnoreOcclusionBit);
    }

    /// <summary>True when the mesh skips the occlusion test.</summary>
    public bool GetIgnoreOcclusion() => HasComponent(Component.MeshRenderer) && (GetInt("CMeshRenderer.renderFlags") & RenderIgnoreOcclusionBit) != 0;

    /// <summary>Biases the mesh's LOD choice, in stops: +1 halves the pixel-error threshold (a finer level is picked sooner), -1 doubles it. Clamped to -8..8.</summary>
    public bool SetLodBias(float stops) =>
        float.IsFinite(stops) && HasComponent(Component.MeshRenderer) && SetFloat("CMeshRenderer.lodBias", Math.Clamp(stops, -8f, 8f));

    /// <summary>The LOD bias in stops (0 when the entity has no mesh).</summary>
    public float GetLodBias() => HasComponent(Component.MeshRenderer) ? GetFloat("CMeshRenderer.lodBias") : 0f;
}
