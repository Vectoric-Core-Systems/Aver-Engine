// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// FogVolume and FogMaterial, ReflectionProbe, LightmapGI, LightmapProbe and VoxelGI, under Godot's property names (docs/plans/fabric/G4_RENDER.md R8).
// FogVolume and ReflectionProbe PUBLISH their values to the engine's environment-effects registry (Aver.Framework EnvFx); the renderer draws the fog volumes
// inside its volumetric fog. The other three keep their properties: Aver has no lightmap baker or sampler and its global illumination is real time.

using Aver.Framework;
using Aver.Scene;

namespace Aver.Fabric;

/// <summary>Godot's RenderingServer enums that the environment nodes use.</summary>
public static partial class RenderingServer
{
    public enum FogVolumeShape { Ellipsoid = 0, Cone = 1, Cylinder = 2, Box = 3, World = 4 }
}

/// <summary>A 3D texture resource. Carried only: fog volumes do not sample a density texture.</summary>
public class Texture3D : Resource
{
}

/// <summary>The calls the environment nodes make on the engine. The engine's is <see cref="EnvFx"/>; a test installs a recording one.</summary>
internal interface IEnvFxBackend
{
    int CreateFogVolume(string key);
    bool DestroyFogVolume(int id);
    bool SetFogVolume(int id, FogVolumeShapeKind shape, float[] world12, float[] halfExtentCm3, float densityPerMetre, float[] albedo3, float[] emission3, float heightFalloff, float edgeFade);
    bool SetFogVolumeEnabled(int id, bool on);
    bool SetFogVolumeShader(int id, string? shaderName) => false;   // G6 SH3 (a default member, so a backend written before it still compiles)
    int CreateProbe(string key);
    bool DestroyProbe(int id);
    bool SetProbeBox(int id, float[] world12, float[] halfExtentCm3, float[] originOffsetCm3);
    bool SetProbeLook(int id, float intensity, float blendDistanceCm, float maxDistanceCm, bool boxProjection, bool interior, bool updateAlways, int resolution);
    bool SetProbeAmbient(int id, int mode, float[] colour3, float energy);
    bool SetProbeMasks(int id, uint cullMask, uint reflectionMask);
    bool SetProbeEnabled(int id, bool on);
    ProbeBlend SelectProbes(float x, float y, float z, float cx, float cy, float cz);
}

internal sealed class NativeEnvFxBackend : IEnvFxBackend
{
    public int CreateFogVolume(string key) => EnvFx.CreateFogVolume(key);
    public bool DestroyFogVolume(int id) => EnvFx.DestroyFogVolume(id);
    public bool SetFogVolume(int id, FogVolumeShapeKind shape, float[] w, float[] h, float d, float[] a, float[] e, float f, float edge) => EnvFx.SetFogVolume(id, shape, w, h, d, a, e, f, edge);
    public bool SetFogVolumeEnabled(int id, bool on) => EnvFx.SetFogVolumeEnabled(id, on);
    public bool SetFogVolumeShader(int id, string? shaderName) => EnvFx.SetFogVolumeShader(id, shaderName);
    public int CreateProbe(string key) => EnvFx.CreateProbe(key);
    public bool DestroyProbe(int id) => EnvFx.DestroyProbe(id);
    public bool SetProbeBox(int id, float[] w, float[] h, float[] o) => EnvFx.SetProbeBox(id, w, h, o);
    public bool SetProbeLook(int id, float i, float b, float m, bool bp, bool interior, bool always, int res) => EnvFx.SetProbeLook(id, i, b, m, bp, interior, always, res);
    public bool SetProbeAmbient(int id, int mode, float[] c, float energy) => EnvFx.SetProbeAmbient(id, mode, c, energy);
    public bool SetProbeMasks(int id, uint cull, uint reflection) => EnvFx.SetProbeMasks(id, cull, reflection);
    public bool SetProbeEnabled(int id, bool on) => EnvFx.SetProbeEnabled(id, on);
    public ProbeBlend SelectProbes(float x, float y, float z, float cx, float cy, float cz) => EnvFx.SelectProbes(x, y, z, cx, cy, cz);
}

internal static class EnvFxBackend
{
    private static readonly NativeEnvFxBackend s_native = new();

    /// <summary>A test's recording backend, or null for the engine's.</summary>
    internal static IEnvFxBackend? Override;

    /// <summary>Where calls go: the override, else the engine when nodes live in it, else nowhere (a headless tree publishes nothing).</summary>
    internal static IEnvFxBackend? Current => Override ?? (NodeHost.Current.IsEngine ? s_native : null);
}

/// <summary>
/// Godot's FogMaterial: what a <see cref="FogVolume"/> adds to the volumetric fog. Density is per metre and may be negative (it carves fog out of the volumes it overlaps).
/// Albedo and emission are sRGB, as in Godot. Changing a value emits <c>changed</c>, which the volume using it republishes.
/// </summary>
public class FogMaterial : Material
{
    private float _density = 1f, _heightFalloff, _edgeFade = 0.1f;
    private Color _albedo = new(1, 1, 1, 1), _emission = new(0, 0, 0, 1);
    private Texture3D? _densityTexture;

    public float Density { get => _density; set { _density = value; EmitChanged(); } }
    public Color Albedo { get => _albedo; set { _albedo = value; EmitChanged(); } }
    public Color Emission { get => _emission; set { _emission = value; EmitChanged(); } }
    /// <summary>The rate, per metre of height above the volume's lowest point, at which the density falls (0 = uniform).</summary>
    public float HeightFalloff { get => _heightFalloff; set { _heightFalloff = value; EmitChanged(); } }
    /// <summary>The softness of the volume's edge, a fraction of its half size (Godot's default 0.1).</summary>
    public float EdgeFade { get => _edgeFade; set { _edgeFade = value; EmitChanged(); } }
    /// <summary>Carried only: the density inside the shape is uniform.</summary>
    public Texture3D? DensityTexture { get => _densityTexture; set { _densityTexture = value; EmitChanged(); } }
}

/// <summary>What a fog volume asks the engine for, in engine terms. Pure, so it can be checked without an engine.</summary>
internal readonly struct FogVolumeFields
{
    public FogVolumeShapeKind Shape { get; init; }
    /// <summary>The instance matrix: three basis rows then the translation, centimetres (<see cref="FabricSpace.ToAverMatrix12"/>).</summary>
    public float[] World { get; init; }
    /// <summary>Half sizes along the engine's local X, Y, Z (Godot size / 2: engine X is Godot -Z, Y is Godot X, Z is Godot Y), centimetres.</summary>
    public float[] Half { get; init; }
    public float DensityPerMetre { get; init; }
    public float[] Albedo { get; init; }
    public float[] Emission { get; init; }
    public float HeightFalloff { get; init; }
    public float EdgeFade { get; init; }
}

/// <summary>
/// A shaped region of fog (Godot's FogVolume). It adds extinction, scattering and emission inside Voxi's volumetric fog, so it shows only while that is on
/// (the project's <c>RENDER.VOLFOG 1</c>, which the converter writes when the start scene's environment enables volumetric fog) - as in Godot, where a FogVolume
/// needs volumetric fog. At most 16 volumes are drawn in a frame, the nearest the camera. The node publishes when it enters the tree, when a property or its
/// material changes, and when its global transform changes.
/// </summary>
public class FogVolume : VisualInstance3D
{
    private Vector3 _size = new(2, 2, 2);
    private RenderingServer.FogVolumeShape _shape = RenderingServer.FogVolumeShape.Box;
    private Material? _material;
    private int _id;
    private Transform3D _published;
    private bool _havePublished;

    public Vector3 Size { get => _size; set { _size = value; Publish(); } }
    public RenderingServer.FogVolumeShape Shape { get => _shape; set { _shape = value; Publish(); } }

    /// <summary>A <see cref="FogMaterial"/>, or a <see cref="ShaderMaterial"/> over a fog shader (G6 SH3), whose DENSITY, ALBEDO and EMISSION replace the material's. Any other
    /// material leaves the volume with Godot's default fog (density 1, white).</summary>
    public Material? Material
    {
        get => _material;
        set
        {
            if (_material != null) _material.Changed -= Publish;
            _material = value;
            if (_material != null) _material.Changed += Publish;
            Publish();
        }
    }

    /// <summary>The engine's id for the volume, 0 when it has none (not in the tree, no host, or the registry refused it).</summary>
    internal int VolumeId => _id;

    internal static IEnvFxBackend? Backend => EnvFxBackend.Current;

    /// <summary>What this volume means in engine terms.</summary>
    internal FogVolumeFields ComputeFields()
    {
        var fm = _material as FogMaterial;
        float[] world = new float[12];
        FabricSpace.ToAverMatrix12(IsInsideTree() ? GlobalTransform : Transform, world);
        Color albedo = (fm?.Albedo ?? new Color(1, 1, 1, 1)).SrgbToLinear();
        Color emission = (fm?.Emission ?? new Color(0, 0, 0, 1)).SrgbToLinear();
        return new FogVolumeFields
        {
            Shape = (FogVolumeShapeKind)(int)_shape,
            World = world,
            Half = new[] { Math.Abs(_size.Z) * 50f, Math.Abs(_size.X) * 50f, Math.Abs(_size.Y) * 50f },
            DensityPerMetre = fm?.Density ?? 1f,
            Albedo = new[] { albedo.R, albedo.G, albedo.B },
            Emission = new[] { emission.R, emission.G, emission.B },
            HeightFalloff = fm?.HeightFalloff ?? 0f,
            EdgeFade = fm?.EdgeFade ?? 0.1f,
        };
    }

    internal int PublishCount { get; private set; }

    private string EffectiveKey => IsInsideTree() ? GetPath().ToString() : Name.ToString();

    private void Publish()
    {
        if (!IsInsideTree() || Backend is not { } b) return;
        if (_id == 0) _id = b.CreateFogVolume(EffectiveKey);
        if (_id == 0) return;
        FogVolumeFields f = ComputeFields();
        b.SetFogVolume(_id, f.Shape, f.World, f.Half, f.DensityPerMetre, f.Albedo, f.Emission, f.HeightFalloff, f.EdgeFade);
        b.SetFogVolumeEnabled(_id, IsVisibleInTree());
        // G6 SH3: a ShaderMaterial over a converted fog shader replaces density, albedo and emission.
        if (_material is ShaderMaterial { EnvironmentKind: "fog" } sm && sm.RegisterWithEngine()) b.SetFogVolumeShader(_id, sm.EnvName);
        else b.SetFogVolumeShader(_id, null);
        _published = GlobalTransform;
        _havePublished = true;
        PublishCount++;
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        if (_material != null) { _material.Changed -= Publish; _material.Changed += Publish; }
        Publish();
    }

    internal override void OnExitingTree()
    {
        if (_material != null) _material.Changed -= Publish;
        if (_id != 0) Backend?.DestroyFogVolume(_id);
        _id = 0;
        _havePublished = false;
        base.OnExitingTree();
    }

    internal override void OnVisibilityApplied(bool effective)
    {
        if (_id != 0) Backend?.SetFogVolumeEnabled(_id, effective);
    }

    // A volume under a moving parent (a platform, a vehicle) follows it: the global transform is compared once a frame and republished when it changed.
    internal override bool UsesInternalProcess => true;

    internal override void InternalProcess(double delta)
    {
        if (_id == 0 || !_havePublished) return;
        Transform3D now = GlobalTransform;
        if (!now.Equals(_published)) Publish();
    }
}

/// <summary>Godot's ReflectionProbe. The probe's box, blend, intensity, box projection, interior and ambient settings are published to the engine, which answers
/// <see cref="Select"/> and <see cref="ProjectDirection"/> from them. The renderers capture and sample no probe cubemap yet (docs/plans/fabric/G4_RENDER.md R8), so
/// the node changes no pixel: surfaces keep Aver's own reflections.</summary>
public class ReflectionProbe : VisualInstance3D
{
    public enum UpdateModeEnum { Once = 0, Always = 1 }
    public enum AmbientModeEnum { Disabled = 0, Environment = 1, Color = 2 }

    private UpdateModeEnum _update;
    private float _intensity = 1f, _maxDistance, _blend = 1f, _ambientEnergy = 1f, _meshLod = 1f;
    private Vector3 _size = new(20, 20, 20), _origin = Vector3.Zero;
    private bool _box, _interior, _shadows;
    private uint _cullMask = 0xFFFFFu, _reflectionMask = 0xFFFFFu;
    private AmbientModeEnum _ambient = AmbientModeEnum.Environment;
    private Color _ambientColor = new(0, 0, 0, 1);
    private int _id;
    private Transform3D _published;
    private bool _havePublished;

    private static readonly Dictionary<int, ReflectionProbe> s_byId = new();

    public UpdateModeEnum UpdateMode { get => _update; set { _update = value; Publish(); } }
    public float Intensity { get => _intensity; set { _intensity = value; Publish(); } }
    /// <summary>Metres from the camera beyond which the probe is not used (0 = always).</summary>
    public float MaxDistance { get => _maxDistance; set { _maxDistance = value; Publish(); } }
    public Vector3 Size { get => _size; set { _size = value; Publish(); } }
    public Vector3 OriginOffset { get => _origin; set { _origin = value; Publish(); } }
    public bool BoxProjection { get => _box; set { _box = value; Publish(); } }
    public bool Interior { get => _interior; set { _interior = value; Publish(); } }
    public bool EnableShadows { get => _shadows; set { _shadows = value; Publish(); } }
    public uint CullMask { get => _cullMask; set { _cullMask = value; Publish(); } }
    public uint ReflectionMask { get => _reflectionMask; set { _reflectionMask = value; Publish(); } }
    public float MeshLodThreshold { get => _meshLod; set { _meshLod = value; Publish(); } }
    /// <summary>Metres: the fade inside the box walls.</summary>
    public float BlendDistance { get => _blend; set { _blend = value; Publish(); } }
    public AmbientModeEnum AmbientMode { get => _ambient; set { _ambient = value; Publish(); } }
    public Color AmbientColor { get => _ambientColor; set { _ambientColor = value; Publish(); } }
    public float AmbientColorEnergy { get => _ambientEnergy; set { _ambientEnergy = value; Publish(); } }

    internal int ProbeId => _id;
    internal int PublishCount { get; private set; }
    private static IEnvFxBackend? Backend => EnvFxBackend.Current;
    private string EffectiveKey => IsInsideTree() ? GetPath().ToString() : Name.ToString();

    private void Publish()
    {
        if (!IsInsideTree() || Backend is not { } b) return;
        if (_id == 0)
        {
            _id = b.CreateProbe(EffectiveKey);
            if (_id != 0) s_byId[_id] = this;
        }
        if (_id == 0) return;
        float[] world = new float[12];
        FabricSpace.ToAverMatrix12(GlobalTransform, world);
        // Engine local X is Godot -Z, Y is Godot X, Z is Godot Y; the offset is along the probe's own axes, so it permutes the same way (X gets -z).
        float[] half = { Math.Abs(_size.Z) * 50f, Math.Abs(_size.X) * 50f, Math.Abs(_size.Y) * 50f };
        float[] origin = { -_origin.Z * 100f, _origin.X * 100f, _origin.Y * 100f };
        b.SetProbeBox(_id, world, half, origin);
        b.SetProbeLook(_id, _intensity, Math.Max(_blend, 0f) * 100f, Math.Max(_maxDistance, 0f) * 100f, _box, _interior, _update == UpdateModeEnum.Always, 256);
        Color amb = _ambientColor.SrgbToLinear();
        b.SetProbeAmbient(_id, (int)_ambient, new[] { amb.R, amb.G, amb.B }, _ambientEnergy);
        b.SetProbeMasks(_id, _cullMask, _reflectionMask);
        b.SetProbeEnabled(_id, IsVisibleInTree());
        _published = GlobalTransform;
        _havePublished = true;
        PublishCount++;
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Publish();
    }

    internal override void OnExitingTree()
    {
        if (_id != 0)
        {
            Backend?.DestroyProbe(_id);
            s_byId.Remove(_id);
        }
        _id = 0;
        _havePublished = false;
        base.OnExitingTree();
    }

    internal override void OnVisibilityApplied(bool effective)
    {
        if (_id != 0) Backend?.SetProbeEnabled(_id, effective);
    }

    internal override bool UsesInternalProcess => true;

    internal override void InternalProcess(double delta)
    {
        if (_id == 0 || !_havePublished) return;
        if (!GlobalTransform.Equals(_published)) Publish();
    }

    /// <summary>The probes that light a point (Godot metres, world space) seen from a camera position, smallest box first, with their share of the blend; the
    /// share left for the sky is <paramref name="rest"/>. At most four. Empty without an engine.</summary>
    public static (ReflectionProbe Probe, float Weight)[] Select(Vector3 point, Vector3 camera, out float rest)
    {
        rest = 1f;
        if (Backend is not { } b) return Array.Empty<(ReflectionProbe, float)>();
        Vec3 p = FabricSpace.ToAverPosition(point), c = FabricSpace.ToAverPosition(camera);
        ProbeBlend blend = b.SelectProbes(p.X, p.Y, p.Z, c.X, c.Y, c.Z);
        rest = blend.Rest;
        var list = new List<(ReflectionProbe, float)>();
        for (int i = 0; i < blend.Ids.Length; i++)
            if (s_byId.TryGetValue(blend.Ids[i], out ReflectionProbe? probe)) list.Add((probe, blend.Weights[i]));
        return list.ToArray();
    }

    /// <summary>The direction (Godot space, unit) to look this probe's capture up with for a reflection ray from a point: box projection applied when the probe has it.</summary>
    public Vector3 ProjectDirection(Vector3 point, Vector3 direction)
    {
        if (_id == 0) return direction.Normalized();
        Vec3 p = FabricSpace.ToAverPosition(point), d = FabricSpace.ToAverDirection(direction);
        var o = EnvFx.ProjectProbeDirection(_id, p.X, p.Y, p.Z, d.X, d.Y, d.Z);
        return FabricSpace.FromAverDirection(new Vec3(o.X, o.Y, o.Z));
    }
}

/// <summary>Godot's baked lightmap resource (a .lmbake file). The converter does not read it; Aver has no lightmap sampler.</summary>
public class LightmapGIData : Resource
{
}

/// <summary>Godot's LightmapGI. Properties are kept. Aver has no lightmap baker and its renderers sample no baked lightmap, so the node bakes nothing and draws
/// nothing: the light a lightmap held comes from Aver's real-time global illumination (the project's RENDER.GI settings).</summary>
public class LightmapGI : VisualInstance3D
{
    public enum BakeQualityEnum { Low = 0, Medium = 1, High = 2, Ultra = 3 }
    public enum BakeErrorEnum { Ok = 0, NoSceneRoot = 1, ForeignData = 2, NoLightmapper = 3, NoSaveRoot = 4, InvalidMesh = 5, MeshSizeTooSmall = 6, TextureSizeTooSmall = 7, TooManyOccluders = 8, LightmappingFailed = 9, UserAborted = 10, TextureSizeTooBig = 11 }
    public enum GenerateProbesEnum { Disabled = 0, Subdiv4 = 1, Subdiv8 = 2, Subdiv16 = 3, Subdiv32 = 4 }
    public enum EnvironmentModeEnum { Disabled = 0, Scene = 1, CustomSky = 2, CustomColor = 3 }

    public BakeQualityEnum Quality { get; set; } = BakeQualityEnum.Medium;
    public bool Supersampling { get; set; }
    public float SupersamplingFactor { get; set; } = 2f;
    public int Bounces { get; set; } = 3;
    public float BounceIndirectEnergy { get; set; } = 1f;
    public bool Directional { get; set; }
    public bool Interior { get; set; }
    public bool UseTextureForBounces { get; set; } = true;
    public int MaxTextureSize { get; set; } = 16384;
    public bool UseDenoiser { get; set; } = true;
    public float DenoiserStrength { get; set; } = 0.1f;
    public int DenoiserRange { get; set; } = 10;
    public float Bias { get; set; } = 0.0005f;
    public float TexelScale { get; set; } = 1f;
    public GenerateProbesEnum GenerateProbesSubdiv { get; set; } = GenerateProbesEnum.Subdiv4;
    public EnvironmentModeEnum EnvironmentMode { get; set; } = EnvironmentModeEnum.Scene;
    public Resource? EnvironmentCustomSky { get; set; }
    public Color EnvironmentCustomColor { get; set; } = new(1, 1, 1, 1);
    public float EnvironmentCustomEnergy { get; set; } = 1f;
    public Resource? CameraAttributes { get; set; }
    public LightmapGIData? LightData { get; set; }

    /// <summary>Always false: a lightmap is never baked or used.</summary>
    public bool IsBaked => false;
}

/// <summary>Godot's LightmapProbe: a position that samples the baked lightmap for dynamic objects. Without a lightmap it only marks a place.</summary>
public class LightmapProbe : Node3D
{
}

/// <summary>Godot's baked VoxelGI resource. Not read; Aver's voxel GI is real time.</summary>
public class VoxelGIData : Resource
{
}

/// <summary>Godot's VoxelGI. Properties are kept and used for nothing: Aver's voxel / ray-traced global illumination is always real time, around the camera (the
/// project's RENDER.GI, RENDER.VOXELRES and RENDER.GIDISTANCE).</summary>
public class VoxelGI : VisualInstance3D
{
    public enum SubdivEnum { Subdiv64 = 0, Subdiv128 = 1, Subdiv256 = 2, Subdiv512 = 3, Max = 4 }

    public SubdivEnum Subdiv { get; set; } = SubdivEnum.Subdiv128;
    public Vector3 Size { get; set; } = new(20, 20, 20);
    public Resource? CameraAttributes { get; set; }
    public VoxelGIData? Data { get; set; }

    public bool IsBaked => false;
}
