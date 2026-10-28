// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// EnvironmentTests: WorldEnvironment, Environment, Sky, CameraAttributes (G4 R3). The classes under Godot's names and defaults, the Object(...) text the
// converter writes building them, the mapping to the engine's post keys / fog / sky light (the same numbers as the converter's FabricEnvTest), and the
// forwarding rule: nothing is pushed at load, a change after load is.

using Aver.Fabric;
using Env = Aver.Fabric.Environment;

static class EnvironmentTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("environment: the classes and Godot's defaults (G4 R3)", Classes);
        T.Run("environment: Object(...) text builds the resource tree (G4 R3)", ObjectText);
        T.Run("environment: the values pushed to the engine (G4 R3)", Mapping);
        T.Run("environment: nothing is pushed at load, a change after load is (G4 R3)", Forwarding);
    });

    private static void Classes()
    {
        foreach (string name in new[] { "WorldEnvironment", "Environment", "Sky", "SkyMaterial", "ProceduralSkyMaterial", "PhysicalSkyMaterial", "PanoramaSkyMaterial",
                                        "CameraAttributes", "CameraAttributesPractical", "CameraAttributesPhysical" })
            T.Check($"ClassDB knows {name}", ClassDB.ClassExists(name));
        T.Check("Environment and Sky resolve to Aver.Fabric's classes, not System.Environment or the framework's static Sky",
            ClassDB.TypeOf("Environment") == typeof(Env) && ClassDB.TypeOf("Sky") == typeof(Aver.Fabric.Sky));
        T.Check("the class tree: PanoramaSkyMaterial is a SkyMaterial is a Material; CameraAttributesPractical a CameraAttributes",
            ClassDB.IsParentClass("PanoramaSkyMaterial", "SkyMaterial") && ClassDB.IsParentClass("SkyMaterial", "Material") && ClassDB.IsParentClass("CameraAttributesPractical", "CameraAttributes"));
        T.Check("WorldEnvironment is a Node", ClassDB.IsParentClass("WorldEnvironment", "Node"));

        var e = new Env();
        T.Check("Environment's defaults: clear background, linear tonemap, exposure 1, white 1, no fog, no glow, no adjustments",
            e.BackgroundMode == Env.BGMode.Clear && e.TonemapMode == Env.ToneMapper.Linear && e.TonemapExposure == 1f && e.TonemapWhite == 1f && !e.FogEnabled &&
            !e.GlowEnabled && !e.AdjustmentEnabled && e.AdjustmentBrightness == 1f && e.AdjustmentContrast == 1f && e.AdjustmentSaturation == 1f);
        T.Check("fog and glow defaults: density 0.01, depth 10 to 100, glow intensity 0.8", e.FogDensity == 0.01f && e.FogDepthBegin == 10f && e.FogDepthEnd == 100f && e.GlowIntensity == 0.8f);
        var p = new CameraAttributesPractical();
        T.Check("CameraAttributesPractical: multiplier 1, no auto exposure, blur amount 0.1, far 10 m, near 2 m",
            p.ExposureMultiplier == 1f && !p.AutoExposureEnabled && p.DofBlurAmount == 0.1f && p.DofBlurFarDistance == 10f && p.DofBlurNearDistance == 2f);
        var pm = new ProceduralSkyMaterial();
        T.Check("ProceduralSkyMaterial: energy 1, curve 0.15", pm.SkyEnergyMultiplier == 1f && pm.SkyCurve == 0.15f);
        T.Check("the enum values are Godot's: AgX is 4, Sky is 2, Disabled ambient is 1", (int)Env.ToneMapper.Agx == 4 && (int)Env.BGMode.Sky == 2 && (int)Env.AmbientSource.Disabled == 1);
    }

    private static void ObjectText()
    {
        Variant v = VariantText.Parse(
            "Object(Environment, \"background_mode\": 2, \"tonemap_mode\": 4, \"tonemap_exposure\": 2.0, \"tonemap_white\": 6.0, \"fog_enabled\": true, \"fog_density\": 0.02, " +
            "\"adjustment_enabled\": true, \"adjustment_saturation\": 0.8, \"ssao_enabled\": true, " +
            "\"sky\": Object(Sky, \"sky_material\": Object(ProceduralSkyMaterial, \"sky_top_color\": Color(0.2, 0.4, 0.8, 1), \"sky_energy_multiplier\": 2.0)))");
        var e = (Env)v.AsGodotObject()!;
        T.Check("the scalar properties arrive under their Godot names", e.BackgroundMode == Env.BGMode.Sky && e.TonemapMode == Env.ToneMapper.Agx && e.TonemapExposure == 2f &&
            e.TonemapWhite == 6f && e.FogEnabled && T.Near(e.FogDensity, 0.02f) && e.AdjustmentEnabled && T.Near(e.AdjustmentSaturation, 0.8f) && e.SsaoEnabled);
        T.Check("nested objects build: Sky holds a ProceduralSkyMaterial with its colour and energy",
            e.Sky?.SkyMaterial is ProceduralSkyMaterial m && m.SkyEnergyMultiplier == 2f && T.Near(m.SkyTopColor.B, 0.8f));
        Variant c = VariantText.Parse("Object(CameraAttributesPractical, \"exposure_multiplier\": 0.5, \"dof_blur_far_enabled\": true, \"dof_blur_far_distance\": 8.0, \"dof_blur_amount\": 0.2)");
        var ca = (CameraAttributesPractical)c.AsGodotObject()!;
        T.Check("camera attributes build", ca.ExposureMultiplier == 0.5f && ca.DofBlurFarEnabled && ca.DofBlurFarDistance == 8f && T.Near(ca.DofBlurAmount, 0.2f));
        Variant w = VariantText.Parse("Object(PanoramaSkyMaterial, \"energy_multiplier\": 1.5)");
        T.Check("a PanoramaSkyMaterial without its texture builds", w.AsGodotObject() is PanoramaSkyMaterial { EnergyMultiplier: 1.5f, Panorama: null });
    }

    private static float Value(List<(string Key, float Value)> values, string key) => values.First(x => x.Key == key).Value;

    private static void Mapping()
    {
        // The EnvGame fixture's environment: AgX, exposure 2 x camera 0.5, white 6, glow 0.7 over 2.0, adjustments, a far blur at 8 m with amount 0.2.
        var e = new Env
        {
            TonemapMode = Env.ToneMapper.Agx, TonemapExposure = 2f, TonemapWhite = 6f, GlowEnabled = true, GlowIntensity = 0.7f, GlowHdrThreshold = 2f,
            AdjustmentEnabled = true, AdjustmentBrightness = 1.2f, AdjustmentContrast = 1.1f, AdjustmentSaturation = 0.8f,
            FogEnabled = true, FogMode = Env.FogModeEnum.Depth, FogDepthBegin = 20f, FogDepthEnd = 120f,
        };
        var cam = new CameraAttributesPractical { ExposureMultiplier = 0.5f, DofBlurFarEnabled = true, DofBlurFarDistance = 8f, DofBlurAmount = 0.2f };
        var v = EnvironmentMapping.PostValues(e, cam);
        T.Check("tonemap AgX is Aver's 7, with the white point", Value(v, "tonemap") == 7f && Value(v, "tonemapwhite") == 6f);
        T.Check("exposure is the product, eye adaptation is off", T.Near(Value(v, "exposure"), 1f) && Value(v, "autoexposure") == 0f);
        T.Check("glow is bloom with its threshold", T.Near(Value(v, "bloom"), 0.7f) && Value(v, "bloomthreshold") == 2f);
        T.Check("the adjustments", T.Near(Value(v, "adjbrightness"), 1.2f) && T.Near(Value(v, "adjcontrast"), 1.1f) && T.Near(Value(v, "adjsaturation"), 0.8f));
        T.Check("the far blur is a lens at 8 m, f/2, 35 mm", Value(v, "dof") == 1f && T.Near(Value(v, "doffocus"), 800f) && T.Near(Value(v, "dofstop"), 2f) && T.Near(Value(v, "dofmm"), 35f));
        T.Check("depth fog reaches ~95% at the end: 3 / (100 m in cm)", T.Near(EnvironmentMapping.FogDensity(e), 3f / 10000f, 1e-7f));

        var plain = EnvironmentMapping.PostValues(new Env(), null);
        T.Check("a default environment states Godot's: linear tonemap (3), exposure 1, no bloom, identity adjustments, no DOF, eye adaptation off",
            Value(plain, "tonemap") == 3f && Value(plain, "exposure") == 1f && Value(plain, "bloom") == 0f && Value(plain, "adjbrightness") == 1f &&
            Value(plain, "adjcontrast") == 1f && Value(plain, "adjsaturation") == 1f && Value(plain, "dof") == 0f && Value(plain, "autoexposure") == 0f &&
            plain.All(x => x.Key != "doffocus" && x.Key != "bloomthreshold"));
        var auto = EnvironmentMapping.PostValues(new Env(), new CameraAttributesPractical { AutoExposureEnabled = true });
        T.Check("camera attributes with auto exposure turn eye adaptation on", Value(auto, "autoexposure") == 1f);
        var phys = EnvironmentMapping.PostValues(new Env(), new CameraAttributesPhysical { FrustumFocusDistance = 5f, FrustumFocalLength = 50f, ExposureAperture = 2.8f });
        T.Check("a physical lens maps focus, aperture and focal length", Value(phys, "dof") == 1f && T.Near(Value(phys, "doffocus"), 500f) && T.Near(Value(phys, "dofstop"), 2.8f) && T.Near(Value(phys, "dofmm"), 50f));

        T.Check("exponential fog is per metre / 100 per centimetre; volumetric fog counts as distance fog; none is 0",
            T.Near(EnvironmentMapping.FogDensity(new Env { FogEnabled = true, FogDensity = 0.02f }), 0.0002f, 1e-8f) &&
            T.Near(EnvironmentMapping.FogDensity(new Env { VolumetricFogEnabled = true, VolumetricFogDensity = 0.05f }), 0.0005f, 1e-8f) &&
            EnvironmentMapping.FogDensity(new Env()) == 0f);
        T.Check("the sky light is the ambient energy, 0 with ambient light disabled",
            EnvironmentMapping.SkyLight(new Env { AmbientLightEnergy = 0.5f }) == 0.5f && EnvironmentMapping.SkyLight(new Env { AmbientLightSource = Env.AmbientSource.Disabled }) == 0f);
        for (int m = 0; m <= 4; m++)
            T.Check($"Godot tonemap mode {m} is Aver's {3 + m}", EnvironmentMapping.TonemapModeOf((Env.ToneMapper)m) == 3 + m);
    }

    private static void Forwarding()
    {
        var env = new Env { TonemapMode = Env.ToneMapper.Filmic };
        var cam = new CameraAttributesPractical();
        var node = new WorldEnvironment { Name = "Env" };
        node.Environment = env;
        node.CameraAttributes = cam;
        T.Check("before the node is in the tree nothing is pushed", node.PushCount == 0);
        SceneTree.Instance.Root.AddChild(node);
        T.Check("entering the tree pushes nothing: the level already holds these values", node.PushCount == 0);
        env.TonemapExposure = 2f;
        T.Check("a change to the environment after load is forwarded", node.PushCount == 1);
        cam.ExposureMultiplier = 0.5f;
        T.Check("so is a change to the camera attributes", node.PushCount == 2);
        node.Environment = new Env();
        T.Check("replacing the environment pushes the new one", node.PushCount == 3);
        env.FogEnabled = true;
        T.Check("and the old one is no longer watched", node.PushCount == 3);
        node.Environment = null;
        T.Check("with no environment there is nothing to push", node.PushCount == 3);
    }
}
