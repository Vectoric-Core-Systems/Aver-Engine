// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// C# binding for the Voxi render settings module.
using System.Runtime.InteropServices;

namespace Aver.Scripting;

/// <summary>Optional renderer features exposed by the Voxi module.</summary>
public enum VoxiFeature
{
    Msaa = 0,
    GlobalIllumination = 1,
    RayTracing = 2,
    PathTracing = 3,
    /// <summary>Mesh-shader geometry path (needs D3D12 Ultimate).</summary>
    MeshShaders = 4,
}

/// <summary>Whether a feature can actually be used on this machine right now.</summary>
public enum VoxiStatus
{
    /// <summary>Implemented and supported — changing the setting has a real effect.</summary>
    Ready = 0,
    /// <summary>Voxi declares the setting but the renderer cannot do it yet.</summary>
    NotImplemented = 1,
    /// <summary>The GPU/driver cannot do it at all.</summary>
    Unsupported = 2,
}

/// <summary>The quality ladder a Voxi feature is set on.</summary>
public enum VoxiQuality { Off = 0, Low = 1, Medium = 2, High = 3, Epic = 4 }

/// <summary>Reads and writes the global Voxi render settings over the <c>aver_voxi_*</c> C ABI.</summary>
public static class Voxi
{
    private const string Lib = "Aver.Render.Voxi";

    // ---- interop ----
    [DllImport(Lib)] private static extern int aver_voxi_feature_count();
    [DllImport(Lib)] private static extern IntPtr aver_voxi_feature_name(int feature);
    [DllImport(Lib)] private static extern int aver_voxi_feature_status(int feature);
    [DllImport(Lib)] private static extern IntPtr aver_voxi_feature_status_text(int feature);
    [DllImport(Lib)] private static extern int aver_voxi_get_msaa();
    [DllImport(Lib)] private static extern int aver_voxi_set_msaa(int samples);
    [DllImport(Lib)] private static extern int aver_voxi_msaa_mask();
    [DllImport(Lib)] private static extern int aver_voxi_get_quality(int feature);
    [DllImport(Lib)] private static extern int aver_voxi_set_quality(int feature, int quality);
    [DllImport(Lib)] private static extern int aver_voxi_get_voxel_resolution();
    [DllImport(Lib)] private static extern int aver_voxi_set_voxel_resolution(int res);
    [DllImport(Lib)] private static extern float aver_voxi_get_gi_intensity();
    [DllImport(Lib)] private static extern int aver_voxi_set_gi_intensity(float v);
    [DllImport(Lib)] private static extern int aver_voxi_get_gi_update_interval();
    [DllImport(Lib)] private static extern int aver_voxi_set_gi_update_interval(int frames);
    [DllImport(Lib)] private static extern float aver_voxi_get_gi_max_distance();
    [DllImport(Lib)] private static extern int aver_voxi_set_gi_max_distance(float cm);
    [DllImport(Lib)] private static extern int aver_voxi_ray_tracing_tier();
    [DllImport(Lib)] private static extern int aver_voxi_max_msaa();
    [DllImport(Lib)] private static extern int aver_voxi_mesh_shader_tier();
    [DllImport(Lib)] private static extern int aver_voxi_shader_model();
    [DllImport(Lib)] private static extern int aver_voxi_get_mesh_shaders();
    [DllImport(Lib)] private static extern int aver_voxi_set_mesh_shaders(int on);

    /// <summary>Marshals a native string pointer, or "?" when it is null.</summary>
    // UTF-8, NOT ANSI. The ABI is explicit -- scripting_abi.h: "Strings are UTF-8 const char*
    // both ways" -- and every other binding in this tree already decodes that way
    // (Aver.Framework/Native.cs, Aver.Scene/Native.cs). PtrToStringAnsi decodes through the
    // OS ANSI codepage instead, so anything non-ASCII came back mangled. Most of what these
    // return is an ASCII name table where the two agree by luck; aver_pbr_get_texture_path is
    // not -- it carries a path the user typed into a free-text field.
    private static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "?";

    /// <summary>---- feature introspection ----</summary>
    public static int FeatureCount => aver_voxi_feature_count();
    /// <summary>The feature's display name.</summary>
    public static string NameOf(VoxiFeature f) => Str(aver_voxi_feature_name((int)f));
    /// <summary>Whether the feature can be used on this machine.</summary>
    public static VoxiStatus StatusOf(VoxiFeature f) => (VoxiStatus)aver_voxi_feature_status((int)f);
    /// <summary>One line explaining the feature's status.</summary>
    public static string StatusTextOf(VoxiFeature f) => Str(aver_voxi_feature_status_text((int)f));
    /// <summary>True when the feature is Ready.</summary>
    public static bool IsAvailable(VoxiFeature f) => StatusOf(f) == VoxiStatus.Ready;

    // ---- anti-aliasing ----
    /// <summary>MSAA sample count: 1 (off), 2, 4 or 8. Assignment is clamped to what the GPU supports.</summary>
    public static int Msaa
    {
        get => aver_voxi_get_msaa();
        set => aver_voxi_set_msaa(value);
    }

    /// <summary>The MSAA sample counts this GPU actually supports.</summary>
    public static IReadOnlyList<int> SupportedMsaaCounts
    {
        get
        {
            int mask = aver_voxi_msaa_mask();
            var list = new List<int>();
            foreach (int n in new[] { 1, 2, 4, 8 })
                if ((mask & n) != 0) list.Add(n);
            return list;
        }
    }

    // ---- quality-ladder features ----
    /// <summary>The quality a ladder feature is set to.</summary>
    public static VoxiQuality GetQuality(VoxiFeature f) => (VoxiQuality)aver_voxi_get_quality((int)f);
    /// <summary>Returns true if the value was accepted (false when the feature is unavailable).</summary>
    public static bool SetQuality(VoxiFeature f, VoxiQuality q) => aver_voxi_set_quality((int)f, (int)q) != 0;

    /// <summary>Voxel cone traced indirect lighting quality.</summary>
    public static VoxiQuality GlobalIllumination
    {
        get => GetQuality(VoxiFeature.GlobalIllumination);
        set => SetQuality(VoxiFeature.GlobalIllumination, value);
    }
    /// <summary>Hardware ray tracing quality.</summary>
    public static VoxiQuality RayTracing
    {
        get => GetQuality(VoxiFeature.RayTracing);
        set => SetQuality(VoxiFeature.RayTracing, value);
    }
    /// <summary>Reference path tracer quality.</summary>
    public static VoxiQuality PathTracing
    {
        get => GetQuality(VoxiFeature.PathTracing);
        set => SetQuality(VoxiFeature.PathTracing, value);
    }

    // ---- global illumination tunables ----
    /// <summary>Edge length of the cubic voxel grid used by GI (32..512).</summary>
    public static int VoxelResolution
    {
        get => aver_voxi_get_voxel_resolution();
        set => aver_voxi_set_voxel_resolution(value);
    }
    /// <summary>Indirect bounce multiplier.</summary>
    public static float GiIntensity
    {
        get => aver_voxi_get_gi_intensity();
        set => aver_voxi_set_gi_intensity(value);
    }
    /// <summary>Cone trace range in centimetres.</summary>
    public static float GiMaxDistance
    {
        get => aver_voxi_get_gi_max_distance();
        set => aver_voxi_set_gi_max_distance(value);
    }
    /// <summary>
    /// How many frames apart the GI volume is re-voxelised. 1 rebuilds every frame and is
    /// bit-identical to having no amortisation at all; clamped to [1, 8].
    /// </summary>
    /// <remarks>
    /// The one native knob for the cost that dominates under camera motion -- a moving camera takes
    /// the GI update from about 7 ms to about 36 ms -- and it was exported and left unbound, so no
    /// script could reach it. The trade is TEMPORAL, not spatial: indirect light lags scene changes
    /// by up to N-1 frames and a still scene converges to exactly the same image, which is what
    /// makes raising this during a chase and dropping it back a reasonable thing for gameplay to do.
    /// The cone trace itself is unaffected -- it is a per-pixel lookup and still runs every frame.
    /// </remarks>
    public static int GiUpdateInterval
    {
        get => aver_voxi_get_gi_update_interval();
        set => aver_voxi_set_gi_update_interval(value);
    }

    // ---- device capabilities ----
    /// <summary>0 = none, 10 = DXR 1.0, 11 = DXR 1.1.</summary>
    public static int RayTracingTier => aver_voxi_ray_tracing_tier();
    /// <summary>Highest MSAA sample count the GPU supports.</summary>
    public static int MaxMsaa => aver_voxi_max_msaa();
    /// <summary>0 = none, 1 = Mesh Shader Tier 1 (D3D12 Ultimate).</summary>
    public static int MeshShaderTier => aver_voxi_mesh_shader_tier();
    /// <summary>Highest shader model, e.g. 60 = SM 6.0, 65 = SM 6.5.</summary>
    public static int ShaderModel => aver_voxi_shader_model();
    /// <summary>Submit geometry through mesh shaders instead of the classic VS/GS path.</summary>
    public static bool MeshShaders
    {
        get => aver_voxi_get_mesh_shaders() != 0;
        set => aver_voxi_set_mesh_shaders(value ? 1 : 0);
    }
}
