// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// C# binding for the PBR material system.
using System.Runtime.InteropServices;

namespace Aver.Scripting;

/// <summary>The parts of a material, reported individually.</summary>
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

/// <summary>Where texture coordinates come from: the mesh's UV set, or a world-axis projection.</summary>
public enum PbrUvMode { Mesh = 0, WorldAligned = 1 }

/// <summary>Creates and addresses PBR materials over the <c>aver_pbr_*</c> C ABI.</summary>
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
    [DllImport(Lib)] private static extern int aver_pbr_get_uv_mode(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_uv_mode(int m, int mode);
    [DllImport(Lib)] private static extern float aver_pbr_get_uv_tiling(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_uv_tiling(int m, float cmPerTile);
    [DllImport(Lib)] private static extern IntPtr aver_pbr_uv_mode_name(int mode);

    [DllImport(Lib)] private static extern IntPtr aver_pbr_get_texture_path(int m, int slot);
    [DllImport(Lib)] private static extern int aver_pbr_set_texture_path(int m, int slot, [MarshalAs(UnmanagedType.LPStr)] string path);
    [DllImport(Lib)] private static extern long aver_pbr_get_texture_id(int m, int slot);
    [DllImport(Lib)] private static extern int aver_pbr_set_texture_id(int m, int slot, long id);
    [DllImport(Lib)] private static extern int aver_pbr_clear_texture(int m, int slot);

    [DllImport(Lib)] private static extern int aver_pbr_consume_dirty(int m);

    /// <summary>Marshals a native string pointer, or "?" when it is null.</summary>
    private static string Str(IntPtr p) => Marshal.PtrToStringAnsi(p) ?? "?";

    /// <summary>---- feature introspection ----</summary>
    public static int FeatureCount => aver_pbr_feature_count();
    /// <summary>The feature's display name.</summary>
    public static string NameOf(PbrFeature f) => Str(aver_pbr_feature_name((int)f));
    /// <summary>Whether the feature reaches the screen.</summary>
    public static PbrStatus StatusOf(PbrFeature f) => (PbrStatus)aver_pbr_status((int)f);
    /// <summary>One line explaining the feature's status.</summary>
    public static string StatusTextOf(PbrFeature f) => Str(aver_pbr_status_text((int)f));
    /// <summary>True when the feature is Ready.</summary>
    public static bool IsAvailable(PbrFeature f) => StatusOf(f) == PbrStatus.Ready;

    /// <summary>The <c>.ocmat</c> TEX slot name, e.g. <c>metalRough</c>.</summary>
    public static string NameOf(PbrTextureSlot s) => Str(aver_pbr_texture_slot_name((int)s));
    /// <summary>The <c>.ocmat</c> BLEND name, e.g. <c>translucent</c>.</summary>
    public static string NameOf(PbrAlphaMode m) => Str(aver_pbr_alpha_mode_name((int)m));
    /// <summary>The <c>.ocmat</c> UV mode name.</summary>
    public static string NameOf(PbrUvMode m) => Str(aver_pbr_uv_mode_name((int)m));

    // ---- lifetime and enumeration ----
    /// <summary>Creates a material with the glTF default surface. Invalid if none could be created.</summary>
    public static PbrMaterial Create(string name) => new(aver_pbr_create(name));
    /// <summary>Live materials. The index is not stable across a destroy — hold the handle instead.</summary>
    public static int Count => aver_pbr_count();
    /// <summary>The material at an enumeration index.</summary>
    public static PbrMaterial At(int index) => new(aver_pbr_at(index));

    /// <summary>A generational handle to one material, with its properties.</summary>
    public readonly struct PbrMaterial : IEquatable<PbrMaterial>
    {
        /// <summary>Wraps a raw ABI handle.</summary>
        internal PbrMaterial(int handle) => Handle = handle;

        /// <summary>The raw ABI handle. 0 is invalid.</summary>
        public int Handle { get; }

        public bool IsValid => aver_pbr_valid(Handle) != 0;
        /// <summary>Destroys the material. False when the handle was already dead.</summary>
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

        /// <summary>See <see cref="PbrUvMode"/>. Defaults to <see cref="PbrUvMode.Mesh"/>.</summary>
        public PbrUvMode UvMode
        {
            get => (PbrUvMode)aver_pbr_get_uv_mode(Handle);
            set => aver_pbr_set_uv_mode(Handle, (int)value);
        }

        /// <summary>World CENTIMETRES per texture tile. Zero or less is rejected, not clamped.</summary>
        public float UvTiling
        {
            get => aver_pbr_get_uv_tiling(Handle);
            set => aver_pbr_set_uv_tiling(Handle, value);
        }

        // ---- texture references: an authoring path AND an opaque id, neither interpreted here ----
        /// <summary>The authoring path bound to a slot.</summary>
        public string GetTexturePath(PbrTextureSlot slot) => Str(aver_pbr_get_texture_path(Handle, (int)slot));
        /// <summary>Binds an authoring path to a slot.</summary>
        public bool SetTexturePath(PbrTextureSlot slot, string path) => aver_pbr_set_texture_path(Handle, (int)slot, path) != 0;
        /// <summary>Opaque asset id (an ObjectId or an <c>.octex</c> GUID). 0 means unset.</summary>
        public long GetTextureId(PbrTextureSlot slot) => aver_pbr_get_texture_id(Handle, (int)slot);
        /// <summary>Binds an opaque asset id to a slot.</summary>
        public bool SetTextureId(PbrTextureSlot slot, long id) => aver_pbr_set_texture_id(Handle, (int)slot, id) != 0;
        /// <summary>Clears both the path and the id on a slot.</summary>
        public bool ClearTexture(PbrTextureSlot slot) => aver_pbr_clear_texture(Handle, (int)slot) != 0;

        /// <summary>True when this material owes the GPU an upload. Reading it clears it.</summary>
        public bool ConsumeDirty() => aver_pbr_consume_dirty(Handle) != 0;

        public bool Equals(PbrMaterial other) => Handle == other.Handle;
        public override bool Equals(object? obj) => obj is PbrMaterial o && Equals(o);
        public override int GetHashCode() => Handle;
        public override string ToString() => IsValid ? $"{Name} (#{Handle})" : $"<invalid #{Handle}>";
    }
}
