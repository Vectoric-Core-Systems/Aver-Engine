using System.Runtime.InteropServices;

namespace Aver.Scripting;

/// <summary>Parts of a material, reported individually so nothing is offered before it shades.</summary>
public enum PbrFeature
{
    /// <summary>The scalar and vector PARAM block.</summary>
    Factors = 0,
    BaseColorMap = 1,
    MetalRoughMap = 2,
    NormalMap = 3,
    OcclusionMap = 4,
    EmissiveMap = 5,
    AlphaMask = 6,
    AlphaBlend = 7,
}

/// <summary>Whether a material feature actually reaches the screen right now.</summary>
public enum PbrStatus
{
    /// <summary>Authored here and consumed by the renderer.</summary>
    Ready = 0,
    /// <summary>Stored by the material system; no renderer consumes it yet.</summary>
    NotImplemented = 1,
    /// <summary>Cannot be done at all.</summary>
    Unsupported = 2,
}

/// <summary>The glTF metallic-roughness texture set, matching the <c>.ocmat</c> TEX slots.</summary>
public enum PbrTextureSlot
{
    BaseColor = 0,
    MetalRough = 1,
    Normal = 2,
    Occlusion = 3,
    Emissive = 4,
}

/// <summary>How base colour alpha is interpreted (<c>.ocmat</c> BLEND).</summary>
public enum PbrAlphaMode { Opaque = 0, Mask = 1, Blend = 2 }

/// <summary>
/// C# binding for the PBR material system (<c>Aver.Render.PBR.dll</c>, C ABI <c>aver_pbr_*</c>).
///
/// Unlike <see cref="Voxi"/>, which is one global settings block, materials are INSTANCES — so the
/// entry points here create and address them by handle. Check <see cref="StatusOf"/> before
/// offering a feature to a user: everything currently reports NotImplemented.
/// </summary>
public static class Pbr
{
    private const string Lib = "Aver.Render.PBR";

    // ---- interop ----
    [DllImport(Lib)] private static extern int aver_pbr_feature_count();
    [DllImport(Lib)] private static extern IntPtr aver_pbr_feature_name(int feature);
    [DllImport(Lib)] private static extern int aver_pbr_status(int feature);
    [DllImport(Lib)] private static extern IntPtr aver_pbr_status_text(int feature);
    [DllImport(Lib)] private static extern IntPtr aver_pbr_texture_slot_name(int slot);
    [DllImport(Lib)] private static extern IntPtr aver_pbr_alpha_mode_name(int mode);

    [DllImport(Lib)] private static extern int aver_pbr_create([MarshalAs(UnmanagedType.LPStr)] string name);
    [DllImport(Lib)] private static extern int aver_pbr_destroy(int m);
    [DllImport(Lib)] private static extern int aver_pbr_valid(int m);
    [DllImport(Lib)] private static extern int aver_pbr_count();
    [DllImport(Lib)] private static extern int aver_pbr_at(int index);

    [DllImport(Lib)] private static extern IntPtr aver_pbr_get_name(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_name(int m, [MarshalAs(UnmanagedType.LPStr)] string name);

    [DllImport(Lib)] private static extern int aver_pbr_get_base_color_factor(int m, float[] out4);
    [DllImport(Lib)] private static extern int aver_pbr_set_base_color_factor(int m, float r, float g, float b, float a);
    [DllImport(Lib)] private static extern int aver_pbr_get_emissive_factor(int m, float[] out3);
    [DllImport(Lib)] private static extern int aver_pbr_set_emissive_factor(int m, float r, float g, float b);
    [DllImport(Lib)] private static extern float aver_pbr_get_metallic_factor(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_metallic_factor(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_roughness_factor(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_roughness_factor(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_normal_scale(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_normal_scale(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_occlusion_strength(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_occlusion_strength(int m, float v);

    [DllImport(Lib)] private static extern int aver_pbr_get_alpha_mode(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_alpha_mode(int m, int mode);
    [DllImport(Lib)] private static extern float aver_pbr_get_alpha_cutoff(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_alpha_cutoff(int m, float v);
    [DllImport(Lib)] private static extern int aver_pbr_get_two_sided(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_two_sided(int m, int on);
    [DllImport(Lib)] private static extern int aver_pbr_get_cast_shadow(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_cast_shadow(int m, int on);

    [DllImport(Lib)] private static extern IntPtr aver_pbr_get_texture_path(int m, int slot);
    [DllImport(Lib)] private static extern int aver_pbr_set_texture_path(int m, int slot, [MarshalAs(UnmanagedType.LPStr)] string path);
    [DllImport(Lib)] private static extern long aver_pbr_get_texture_id(int m, int slot);
    [DllImport(Lib)] private static extern int aver_pbr_set_texture_id(int m, int slot, long id);
    [DllImport(Lib)] private static extern int aver_pbr_clear_texture(int m, int slot);

    [DllImport(Lib)] private static extern int aver_pbr_consume_dirty(int m);

    private static string Str(IntPtr p) => Marshal.PtrToStringAnsi(p) ?? "?";

    // ---- feature introspection ----
    public static int FeatureCount => aver_pbr_feature_count();
    public static string NameOf(PbrFeature f) => Str(aver_pbr_feature_name((int)f));
    public static PbrStatus StatusOf(PbrFeature f) => (PbrStatus)aver_pbr_status((int)f);
    public static string StatusTextOf(PbrFeature f) => Str(aver_pbr_status_text((int)f));
    public static bool IsAvailable(PbrFeature f) => StatusOf(f) == PbrStatus.Ready;

    /// <summary>The <c>.ocmat</c> TEX slot name, e.g. <c>metalRough</c>.</summary>
    public static string NameOf(PbrTextureSlot s) => Str(aver_pbr_texture_slot_name((int)s));
    /// <summary>The <c>.ocmat</c> BLEND name, e.g. <c>translucent</c>.</summary>
    public static string NameOf(PbrAlphaMode m) => Str(aver_pbr_alpha_mode_name((int)m));

    // ---- lifetime and enumeration ----
    /// <summary>Creates a material with the glTF default surface. Invalid if none could be created.</summary>
    public static PbrMaterial Create(string name) => new(aver_pbr_create(name));
    /// <summary>Live materials. The index is not stable across a destroy — hold the handle instead.</summary>
    public static int Count => aver_pbr_count();
    public static PbrMaterial At(int index) => new(aver_pbr_at(index));

    /// <summary>
    /// A handle to one material. Carries a generation, so a handle to a destroyed material reports
    /// <see cref="IsValid"/> false rather than addressing whatever reused its slot.
    /// </summary>
    public readonly struct PbrMaterial : IEquatable<PbrMaterial>
    {
        internal PbrMaterial(int handle) => Handle = handle;

        /// <summary>The raw ABI handle. 0 is invalid.</summary>
        public int Handle { get; }

        public bool IsValid => aver_pbr_valid(Handle) != 0;
        public bool Destroy() => aver_pbr_destroy(Handle) != 0;

        public string Name
        {
            get => Str(aver_pbr_get_name(Handle));
            set => aver_pbr_set_name(Handle, value);
        }

        /// <summary>Base colour tint, rgba.</summary>
        public (float R, float G, float B, float A) BaseColorFactor
        {
            get
            {
                var v = new float[4];
                return aver_pbr_get_base_color_factor(Handle, v) != 0 ? (v[0], v[1], v[2], v[3]) : (0, 0, 0, 0);
            }
            set => aver_pbr_set_base_color_factor(Handle, value.R, value.G, value.B, value.A);
        }

        /// <summary>Emissive radiance, rgb. Not a ratio, so it is not clamped to 1.</summary>
        public (float R, float G, float B) EmissiveFactor
        {
            get
            {
                var v = new float[3];
                return aver_pbr_get_emissive_factor(Handle, v) != 0 ? (v[0], v[1], v[2]) : (0, 0, 0);
            }
            set => aver_pbr_set_emissive_factor(Handle, value.R, value.G, value.B);
        }

        public float MetallicFactor
        {
            get => aver_pbr_get_metallic_factor(Handle);
            set => aver_pbr_set_metallic_factor(Handle, value);
        }

        /// <summary>Clamped away from 0: a perfect mirror collapses the GGX denominator.</summary>
        public float RoughnessFactor
        {
            get => aver_pbr_get_roughness_factor(Handle);
            set => aver_pbr_set_roughness_factor(Handle, value);
        }

        public float NormalScale
        {
            get => aver_pbr_get_normal_scale(Handle);
            set => aver_pbr_set_normal_scale(Handle, value);
        }

        public float OcclusionStrength
        {
            get => aver_pbr_get_occlusion_strength(Handle);
            set => aver_pbr_set_occlusion_strength(Handle, value);
        }

        public PbrAlphaMode AlphaMode
        {
            get => (PbrAlphaMode)aver_pbr_get_alpha_mode(Handle);
            set => aver_pbr_set_alpha_mode(Handle, (int)value);
        }

        /// <summary>Read only under <see cref="PbrAlphaMode.Mask"/>.</summary>
        public float AlphaCutoff
        {
            get => aver_pbr_get_alpha_cutoff(Handle);
            set => aver_pbr_set_alpha_cutoff(Handle, value);
        }

        public bool TwoSided
        {
            get => aver_pbr_get_two_sided(Handle) != 0;
            set => aver_pbr_set_two_sided(Handle, value ? 1 : 0);
        }

        public bool CastShadow
        {
            get => aver_pbr_get_cast_shadow(Handle) != 0;
            set => aver_pbr_set_cast_shadow(Handle, value ? 1 : 0);
        }

        // ---- texture references: an authoring path AND an opaque id, neither interpreted here ----
        public string GetTexturePath(PbrTextureSlot slot) => Str(aver_pbr_get_texture_path(Handle, (int)slot));
        public bool SetTexturePath(PbrTextureSlot slot, string path) => aver_pbr_set_texture_path(Handle, (int)slot, path) != 0;
        /// <summary>Opaque asset id (an ObjectId or an <c>.octex</c> GUID). 0 means unset.</summary>
        public long GetTextureId(PbrTextureSlot slot) => aver_pbr_get_texture_id(Handle, (int)slot);
        public bool SetTextureId(PbrTextureSlot slot, long id) => aver_pbr_set_texture_id(Handle, (int)slot, id) != 0;
        public bool ClearTexture(PbrTextureSlot slot) => aver_pbr_clear_texture(Handle, (int)slot) != 0;

        /// <summary>
        /// True when this material still owes the GPU an upload. READING IT CLEARS IT, so exactly
        /// one consumer acts on each change.
        /// </summary>
        public bool ConsumeDirty() => aver_pbr_consume_dirty(Handle) != 0;

        public bool Equals(PbrMaterial other) => Handle == other.Handle;
        public override bool Equals(object? obj) => obj is PbrMaterial o && Equals(o);
        public override int GetHashCode() => Handle;
        public override string ToString() => IsValid ? $"{Name} (#{Handle})" : $"<invalid #{Handle}>";
    }
}
