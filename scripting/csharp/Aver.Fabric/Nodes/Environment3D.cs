// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// WorldEnvironment and the resources it holds (Environment, Sky and its materials, CameraAttributes), under Godot's property names.
// docs/plans/fabric/G4_RENDER.md R3. THE LEVEL IS THE SOURCE OF TRUTH AT LOAD: the converter writes the same values as the level's SKY / FOG
// records and a level-wide post volume, so the node pushes nothing when it enters the tree. A change made after that (a script fading the fog,
// switching the tonemapper) is forwarded: post values to a script post layer, fog and sky light to the sky overrides.

namespace Aver.Fabric;

public class SkyMaterial : Material
{
}

/// <summary>Godot's ProceduralSkyMaterial. The converter turns it into the level's authored dome; these values are what a script reads.</summary>
public class ProceduralSkyMaterial : SkyMaterial
{
    public Color SkyTopColor { get; set; } = new(0.385f, 0.454f, 0.55f, 1f);
    public Color SkyHorizonColor { get; set; } = new(0.6463f, 0.6558f, 0.6708f, 1f);
    public float SkyCurve { get; set; } = 0.15f;
    public float SkyEnergyMultiplier { get; set; } = 1f;
    public Color GroundBottomColor { get; set; } = new(0.2f, 0.169f, 0.133f, 1f);
    public Color GroundHorizonColor { get; set; } = new(0.6463f, 0.6558f, 0.6708f, 1f);
    public float GroundCurve { get; set; } = 0.02f;
    public float GroundEnergyMultiplier { get; set; } = 1f;
    public float SunAngleMax { get; set; } = 30f;
    public float SunCurve { get; set; } = 0.15f;
    public bool UseDebanding { get; set; } = true;
}

public class PhysicalSkyMaterial : SkyMaterial
{
    public float RayleighCoefficient { get; set; } = 2f;
    public Color RayleighColor { get; set; } = new(0.3f, 0.405f, 0.6f, 1f);
    public float MieCoefficient { get; set; } = 0.005f;
    public float MieEccentricity { get; set; } = 0.8f;
    public Color MieColor { get; set; } = new(0.69f, 0.729f, 0.812f, 1f);
    public float Turbidity { get; set; } = 10f;
    public float SunDiskScale { get; set; } = 1f;
    public Color GroundColor { get; set; } = new(0.1f, 0.07f, 0.034f, 1f);
    public float EnergyMultiplier { get; set; } = 1f;
    public bool UseDebanding { get; set; } = true;
}

/// <summary>An equirectangular sky. The renderers do not sample the texture: the converter fits it onto the authored dome.</summary>
public class PanoramaSkyMaterial : SkyMaterial
{
    public Texture2D? Panorama { get; set; }
    public bool Filter { get; set; } = true;
    public float EnergyMultiplier { get; set; } = 1f;
}

public class Sky : Resource
{
    public enum RadianceSizeEnum { Size32, Size64, Size128, Size256, Size512, Size1024, Size2048 }
    public enum ProcessModeEnum { Automatic, Quality, Incremental, Realtime }

    public SkyMaterial? SkyMaterial { get; set; }
    public RadianceSizeEnum RadianceSize { get; set; } = RadianceSizeEnum.Size256;
    public ProcessModeEnum ProcessMode { get; set; } = ProcessModeEnum.Automatic;
}

/// <summary>Godot's Environment. Every property change emits <c>changed</c>; a live WorldEnvironment forwards it to the engine.</summary>
public class Environment : Resource
{
    public enum BGMode { Clear = 0, Color = 1, Sky = 2, Canvas = 3, Keep = 4, CameraFeed = 5 }
    public enum AmbientSource { Bg = 0, Disabled = 1, Color = 2, Sky = 3 }
    public enum ToneMapper { Linear = 0, Reinhard = 1, Filmic = 2, Aces = 3, Agx = 4 }
    public enum FogModeEnum { Exponential = 0, Depth = 1 }

    private BGMode _bgMode; private Color _bgColor = new(0, 0, 0, 1); private float _bgEnergy = 1f; private Sky? _sky;
    private AmbientSource _ambientSource; private Color _ambientColor = new(0, 0, 0, 1); private float _ambientEnergy = 1f;
    private ToneMapper _tonemap; private float _exposure = 1f, _white = 1f;
    private bool _glow; private float _glowIntensity = 0.8f, _glowThreshold = 1f;
    private bool _fog; private FogModeEnum _fogMode; private Color _fogColor = new(0.518f, 0.553f, 0.608f, 1f);
    private float _fogDensity = 0.01f, _fogHeight, _fogHeightDensity, _fogDepthBegin = 10f, _fogDepthEnd = 100f;
    private bool _volFog; private float _volFogDensity = 0.05f; private Color _volFogAlbedo = new(1, 1, 1, 1);
    private bool _adjust; private float _adjBrightness = 1f, _adjContrast = 1f, _adjSaturation = 1f;

    private void Assign<T>(ref T field, T value) { field = value; EmitChanged(); }

    public BGMode BackgroundMode { get => _bgMode; set => Assign(ref _bgMode, value); }
    public Color BackgroundColor { get => _bgColor; set => Assign(ref _bgColor, value); }
    public float BackgroundEnergyMultiplier { get => _bgEnergy; set => Assign(ref _bgEnergy, value); }
    public Sky? Sky { get => _sky; set => Assign(ref _sky, value); }
    public Vector3 SkyRotation { get; set; }
    public float SkyCustomFov { get; set; }
    public AmbientSource AmbientLightSource { get => _ambientSource; set => Assign(ref _ambientSource, value); }
    public Color AmbientLightColor { get => _ambientColor; set => Assign(ref _ambientColor, value); }
    public float AmbientLightEnergy { get => _ambientEnergy; set => Assign(ref _ambientEnergy, value); }
    public float AmbientLightSkyContribution { get; set; } = 1f;

    public ToneMapper TonemapMode { get => _tonemap; set => Assign(ref _tonemap, value); }
    public float TonemapExposure { get => _exposure; set => Assign(ref _exposure, value); }
    public float TonemapWhite { get => _white; set => Assign(ref _white, value); }

    // Kept for scripts and reported by the converter: Aver has no screen-space or SDF version of these (its ray-traced GI covers them).
    public bool SsrEnabled { get; set; }
    public bool SsaoEnabled { get; set; }
    public bool SsilEnabled { get; set; }
    public bool SdfgiEnabled { get; set; }

    public bool GlowEnabled { get => _glow; set => Assign(ref _glow, value); }
    public float GlowIntensity { get => _glowIntensity; set => Assign(ref _glowIntensity, value); }
    public float GlowStrength { get; set; } = 1f;
    public float GlowBloom { get; set; }
    public float GlowHdrThreshold { get => _glowThreshold; set => Assign(ref _glowThreshold, value); }

    public bool FogEnabled { get => _fog; set => Assign(ref _fog, value); }
    public FogModeEnum FogMode { get => _fogMode; set => Assign(ref _fogMode, value); }
    public Color FogLightColor { get => _fogColor; set => Assign(ref _fogColor, value); }
    public float FogLightEnergy { get; set; } = 1f;
    public float FogDensity { get => _fogDensity; set => Assign(ref _fogDensity, value); }
    public float FogAerialPerspective { get; set; }
    public float FogSkyAffect { get; set; } = 1f;
    public float FogHeight { get => _fogHeight; set => Assign(ref _fogHeight, value); }
    public float FogHeightDensity { get => _fogHeightDensity; set => Assign(ref _fogHeightDensity, value); }
    public float FogDepthBegin { get => _fogDepthBegin; set => Assign(ref _fogDepthBegin, value); }
    public float FogDepthEnd { get => _fogDepthEnd; set => Assign(ref _fogDepthEnd, value); }
    public float FogDepthCurve { get; set; } = 1f;
    public bool VolumetricFogEnabled { get => _volFog; set => Assign(ref _volFog, value); }
    public float VolumetricFogDensity { get => _volFogDensity; set => Assign(ref _volFogDensity, value); }
    public Color VolumetricFogAlbedo { get => _volFogAlbedo; set => Assign(ref _volFogAlbedo, value); }

    public bool AdjustmentEnabled { get => _adjust; set => Assign(ref _adjust, value); }
    public float AdjustmentBrightness { get => _adjBrightness; set => Assign(ref _adjBrightness, value); }
    public float AdjustmentContrast { get => _adjContrast; set => Assign(ref _adjContrast, value); }
    public float AdjustmentSaturation { get => _adjSaturation; set => Assign(ref _adjSaturation, value); }
    public Resource? AdjustmentColorCorrection { get; set; }
}

public class CameraAttributes : Resource
{
    private float _multiplier = 1f; private bool _auto;

    public float ExposureSensitivity { get; set; } = 100f;
    public float ExposureMultiplier { get => _multiplier; set { _multiplier = value; EmitChanged(); } }
    public bool AutoExposureEnabled { get => _auto; set { _auto = value; EmitChanged(); } }
    public float AutoExposureScale { get; set; } = 0.4f;
    public float AutoExposureSpeed { get; set; } = 0.5f;
}

public class CameraAttributesPractical : CameraAttributes
{
    private bool _near, _far; private float _nearDist = 2f, _farDist = 10f, _amount = 0.1f;

    public bool DofBlurFarEnabled { get => _far; set { _far = value; EmitChanged(); } }
    public float DofBlurFarDistance { get => _farDist; set { _farDist = value; EmitChanged(); } }
    public float DofBlurFarTransition { get; set; } = 5f;
    public bool DofBlurNearEnabled { get => _near; set { _near = value; EmitChanged(); } }
    public float DofBlurNearDistance { get => _nearDist; set { _nearDist = value; EmitChanged(); } }
    public float DofBlurNearTransition { get; set; } = 1f;
    public float DofBlurAmount { get => _amount; set { _amount = value; EmitChanged(); } }
    public float AutoExposureMinSensitivity { get; set; } = 0.0f;
    public float AutoExposureMaxSensitivity { get; set; } = 800.0f;
}

public class CameraAttributesPhysical : CameraAttributes
{
    private float _focus = 10f, _focal = 35f, _aperture = 16f;

    public float FrustumFocusDistance { get => _focus; set { _focus = value; EmitChanged(); } }
    public float FrustumFocalLength { get => _focal; set { _focal = value; EmitChanged(); } }
    public float FrustumNear { get; set; } = 0.05f;
    public float FrustumFar { get; set; } = 4000f;
    public float ExposureAperture { get => _aperture; set { _aperture = value; EmitChanged(); } }
    public float ExposureShutterSpeed { get; set; } = 100f;
    public float AutoExposureMinExposureValue { get; set; } = -8f;
    public float AutoExposureMaxExposureValue { get; set; } = 10f;
}

/// <summary>The mapping from Godot's environment to the engine's post volume keys, fog and sky light. Pure, so it is testable; the converter
/// (tools/fabric, <c>aver/formats/GodotEnvironment.hpp</c>) writes the same numbers into the level.</summary>
public static class EnvironmentMapping
{
    /// <summary>Aver's tonemap mode for Godot's tonemap_mode: 3 + mode.</summary>
    public static int TonemapModeOf(Environment.ToneMapper mode) => 3 + Math.Clamp((int)mode, 0, 4);

    /// <summary>The fog density per centimetre for a density per metre.</summary>
    public static float FogDensityPerCm(float perMetre) => Math.Max(perMetre, 0f) / 100f;

    /// <summary>Godot's tonemap_exposure times the camera attributes' exposure multiplier.</summary>
    public static float Exposure(float tonemapExposure, float cameraMultiplier) => Math.Max(tonemapExposure, 0f) * Math.Max(cameraMultiplier, 0f);

    /// <summary>The POSTVOLUME key values this environment states, in the order the converter writes them.</summary>
    public static List<(string Key, float Value)> PostValues(Environment env, CameraAttributes? camera)
    {
        var v = new List<(string, float)>
        {
            ("tonemap", TonemapModeOf(env.TonemapMode)),
            ("tonemapwhite", Math.Max(env.TonemapWhite, 1e-3f)),
            ("exposure", Exposure(env.TonemapExposure, camera?.ExposureMultiplier ?? 1f)),
            ("autoexposure", camera != null && camera.AutoExposureEnabled ? 1f : 0f),
            ("bloom", env.GlowEnabled ? env.GlowIntensity : 0f),
        };
        if (env.GlowEnabled && env.GlowHdrThreshold != 1f) v.Add(("bloomthreshold", env.GlowHdrThreshold));
        v.Add(("adjbrightness", env.AdjustmentEnabled ? env.AdjustmentBrightness : 1f));
        v.Add(("adjcontrast", env.AdjustmentEnabled ? env.AdjustmentContrast : 1f));
        v.Add(("adjsaturation", env.AdjustmentEnabled ? env.AdjustmentSaturation : 1f));
        bool dof = false; float focusCm = 1000f, stop = 4f, mm = 35f;
        switch (camera)
        {
            case CameraAttributesPractical p when p.DofBlurFarEnabled || p.DofBlurNearEnabled:
                dof = true;
                focusCm = Math.Max(p.DofBlurFarEnabled ? p.DofBlurFarDistance : p.DofBlurNearDistance, 0.1f) * 100f;
                stop = Math.Clamp(0.4f / Math.Max(p.DofBlurAmount, 1e-3f), 0.7f, 32f);
                break;
            case CameraAttributesPhysical ph:
                dof = true;
                focusCm = Math.Max(ph.FrustumFocusDistance, 0.1f) * 100f;
                stop = Math.Clamp(ph.ExposureAperture, 0.7f, 32f);
                mm = Math.Clamp(ph.FrustumFocalLength, 10f, 300f);
                break;
        }
        v.Add(("dof", dof ? 1f : 0f));
        if (dof) { v.Add(("doffocus", focusCm)); v.Add(("dofstop", stop)); v.Add(("dofmm", mm)); }
        return v;
    }

    /// <summary>The fog density per centimetre the environment states (0 when fog is off); volumetric fog counts as distance fog.</summary>
    public static float FogDensity(Environment env)
    {
        if (env.FogEnabled)
        {
            if (env.FogMode == Environment.FogModeEnum.Depth)
                return 3f / (Math.Max(env.FogDepthEnd - env.FogDepthBegin, 0.1f) * 100f);
            return FogDensityPerCm(env.FogDensity);
        }
        return env.VolumetricFogEnabled ? FogDensityPerCm(env.VolumetricFogDensity) : 0f;
    }

    /// <summary>The sky light multiplier: 0 with ambient light disabled, else the ambient energy.</summary>
    public static float SkyLight(Environment env) => env.AmbientLightSource == Environment.AmbientSource.Disabled ? 0f : env.AmbientLightEnergy;
}

/// <summary>Godot's WorldEnvironment. It holds the Environment and CameraAttributes; at load the level already carries their values, so nothing
/// is pushed until one of them changes.</summary>
public class WorldEnvironment : Node
{
    /// <summary>The script post layer the node writes to.</summary>
    public const string Layer = "fabric.environment";

    private Environment? _environment;
    private CameraAttributes? _camera;
    private bool _live;

    public Environment? Environment
    {
        get => _environment;
        set { Rewire(_environment, value); _environment = value; Push(); }
    }

    public CameraAttributes? CameraAttributes
    {
        get => _camera;
        set { Rewire(_camera, value); _camera = value; Push(); }
    }

    private void Rewire(Resource? old, Resource? next)
    {
        if (old != null) old.Changed -= Push;
        if (next != null) next.Changed += Push;
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        _live = true;   // what follows is a change, not the load
    }

    internal int PushCount { get; private set; }

    /// <summary>Forwards the current values to the engine: the post layer, the fog and the sky light. Does nothing before the node is in the
    /// tree, outside the engine, or with no Environment.</summary>
    internal void Push()
    {
        if (!_live || _environment == null) return;
        PushCount++;
        if (!NodeHost.Current.IsEngine) return;
        foreach ((string key, float value) in EnvironmentMapping.PostValues(_environment, _camera))
            Aver.Framework.PostFx.SetValue(Layer, key, value);
        Aver.Framework.PostFx.SetLayerWeight(Layer, 1f, 0f);
        float density = EnvironmentMapping.FogDensity(_environment);
        Aver.Framework.Sky.SetFogDensity(density);
        // The environment's fog colour is a nominal sRGB value; the fog override takes linear light.
        Color c = _environment.FogEnabled ? _environment.FogLightColor : _environment.VolumetricFogAlbedo;
        Aver.Framework.Sky.SetFogColor(MathF.Pow(c.R, 2.2f), MathF.Pow(c.G, 2.2f), MathF.Pow(c.B, 2.2f));
        if (_environment.FogEnabled && _environment.FogHeightDensity != 0f)
            Aver.Framework.Sky.SetFogShape(Math.Abs(_environment.FogHeightDensity) / 100f, _environment.FogHeight * 100f, 0f);
        Aver.Framework.Sky.SetSkyLight(EnvironmentMapping.SkyLight(_environment));
    }
}
