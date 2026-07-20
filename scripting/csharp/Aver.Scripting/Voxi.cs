using System.Runtime.InteropServices;

namespace Aver.Scripting;

/// <summary>Optional renderer features exposed by the Voxi module.</summary>
public enum VoxiFeature
{
    Msaa = 0,
    GlobalIllumination = 1,
    RayTracing = 2,
    PathTracing = 3,
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

public enum VoxiQuality { Off = 0, Low = 1, Medium = 2, High = 3, Epic = 4 }

/// <summary>
/// C# binding for the Voxi render module (<c>Aver.Render.Voxi.dll</c>, C ABI <c>aver_voxi_*</c>).
///
/// Setters are honest: assigning a feature the device or renderer cannot do leaves the value at
/// Off. Check <see cref="StatusOf"/> before offering a setting to a user.
/// </summary>
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
    [DllImport(Lib)] private static extern float aver_voxi_get_gi_max_distance();
    [DllImport(Lib)] private static extern int aver_voxi_set_gi_max_distance(float cm);
    [DllImport(Lib)] private static extern int aver_voxi_ray_tracing_tier();
    [DllImport(Lib)] private static extern int aver_voxi_max_msaa();

    private static string Str(IntPtr p) => Marshal.PtrToStringAnsi(p) ?? "?";

    // ---- feature introspection ----
    public static int FeatureCount => aver_voxi_feature_count();
    public static string NameOf(VoxiFeature f) => Str(aver_voxi_feature_name((int)f));
    public static VoxiStatus StatusOf(VoxiFeature f) => (VoxiStatus)aver_voxi_feature_status((int)f);
    public static string StatusTextOf(VoxiFeature f) => Str(aver_voxi_feature_status_text((int)f));
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

    // ---- device capabilities ----
    /// <summary>0 = none, 10 = DXR 1.0, 11 = DXR 1.1.</summary>
    public static int RayTracingTier => aver_voxi_ray_tracing_tier();
    /// <summary>Highest MSAA sample count the GPU supports.</summary>
    public static int MaxMsaa => aver_voxi_max_msaa();
}
