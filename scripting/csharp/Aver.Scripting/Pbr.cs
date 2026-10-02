// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
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

    [DllImport(Lib)] private static extern int aver_pbr_create([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] private static extern int aver_pbr_destroy(int m);
    [DllImport(Lib)] private static extern int aver_pbr_valid(int m);
    [DllImport(Lib)] private static extern int aver_pbr_count();
    [DllImport(Lib)] private static extern int aver_pbr_at(int index);

    [DllImport(Lib)] private static extern IntPtr aver_pbr_get_name(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_name(int m, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

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
    [DllImport(Lib)] private static extern float aver_pbr_get_reflectance(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_reflectance(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_f90(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_f90(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_ior(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_ior(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_transmission(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_transmission(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_subsurface_weight(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_subsurface_weight(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_coat_weight(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_coat_weight(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_coat_roughness(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_coat_roughness(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_coat_f0(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_coat_f0(int m, float v);
    [DllImport(Lib)] private static extern float aver_pbr_get_subsurface_radius(int m);
    [DllImport(Lib)] private static extern int aver_pbr_set_subsurface_radius(int m, float v);
    [DllImport(Lib)] private static extern int aver_pbr_get_subsurface_color(int m, float[] out3);
    [DllImport(Lib)] private static extern int aver_pbr_set_subsurface_color(int m, float r, float g, float b);

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
    [DllImport(Lib)] private static extern int aver_pbr_set_texture_path(int m, int slot, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib)] private static extern long aver_pbr_get_texture_id(int m, int slot);
    [DllImport(Lib)] private static extern int aver_pbr_set_texture_id(int m, int slot, long id);
    [DllImport(Lib)] private static extern int aver_pbr_clear_texture(int m, int slot);

    [DllImport(Lib)] private static extern int aver_pbr_consume_dirty(int m);

    /// <summary>Marshals a native string pointer, or "?" when it is null.</summary>
    // UTF-8, NOT ANSI. The ABI is explicit -- scripting_abi.h: "Strings are UTF-8 const char*
    // both ways" -- and every other binding in this tree already decodes that way
    // (Aver.Framework/Native.cs, Aver.Scene/Native.cs). PtrToStringAnsi decodes through the
    // OS ANSI codepage instead, so anything non-ASCII came back mangled. Most of what these
    // return is an ASCII name table where the two agree by luck; aver_pbr_get_texture_path is
    // not -- it carries a path the user typed into a free-text field.
    private static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "?";

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

        /// <summary>
        /// Normal-incidence reflectance of the DIELECTRIC substrate, in [0,1]. 0.04 is the default
        /// and is what almost every non-metal actually measures; raising it is how a surface reads
        /// as gemstone or wet rather than as plastic.
        /// </summary>
        /// <remarks>
        /// This is the F0 the multiple-scattering compensation uses for the diffuse lobe, so it
        /// changes how much energy reaches diffuse as well as how bright the highlight is -- see
        /// averIndirectTerms in material_prelude.hlsl. It has nothing to do with Metallic: a metal
        /// takes its F0 from the base colour instead.
        /// </remarks>
        public float Reflectance
        {
            get => aver_pbr_get_reflectance(Handle);
            set => aver_pbr_set_reflectance(Handle, value);
        }

        /// <summary>
        /// Reflectance at grazing incidence, in [0,1]. 1.0 is the physical answer for a clean
        /// surface and is the default; lowering it is a stylistic choice that softens the rim
        /// every Fresnel term produces.
        /// </summary>
        public float F90
        {
            get => aver_pbr_get_f90(Handle);
            set => aver_pbr_set_f90(Handle, value);
        }

        /// <summary>
        /// Refractive index of the substrate. 1.0 is vacuum and the floor; water is about 1.33,
        /// window glass about 1.5, diamond about 2.42.
        ///
        /// NOT read by any shading term today, and this doc used to claim two consumers it does not
        /// have. It said ior sets the critical angle for total internal reflection: the function
        /// that did so was removed, because a parallel-sided pane seen from outside can never total-
        /// internally-reflect (Snell bounds the internal angle at asin(1/n), the critical angle
        /// itself). And it said <see cref="Reflectance"/> is derived from ior: it is not — the two
        /// are authored independently, and nothing reconciles them.
        ///
        /// It is parsed, packed, uploaded and drivable from a material graph pin, so setting it is
        /// not lost; it simply does not change what you see yet. Set <see cref="Reflectance"/> for
        /// that.
        /// </summary>
        public float Ior
        {
            get => aver_pbr_get_ior(Handle);
            set => aver_pbr_set_ior(Handle, value);
        }

        /// <summary>
        /// [0,1] how optically see-through the substrate is, independent of where the camera stands.
        /// It pulls blended coverage down toward (1 - transmission) before the view-angle Fresnel
        /// term lifts it back at grazing angles, and it scales the diffuse lobe so a transmissive
        /// surface does not also scatter its full base colour back at the viewer.
        ///
        /// This is NOT refraction: light does not bend passing through the surface.
        /// </summary>
        public float Transmission
        {
            get => aver_pbr_get_transmission(Handle);
            set => aver_pbr_set_transmission(Handle, value);
        }

        /// <summary>
        /// [0,1] how far light wraps past the terminator. 0 turns the approximation off entirely —
        /// see <see cref="SubsurfaceRadius"/> for what the pair together can and cannot do.
        /// </summary>
        public float SubsurfaceWeight
        {
            get => aver_pbr_get_subsurface_weight(Handle);
            set => aver_pbr_set_subsurface_weight(Handle, value);
        }

        /// <summary>
        /// [0,1] how much clear coat sits over the base material — car paint, varnish, a wet stone.
        /// 0 turns the whole coat off: no flag, no lobe, nothing computed.
        ///
        /// Authored per material, but whether the renderer evaluates it at all is a PROJECT-wide
        /// decision (RENDER.LAYEREDBSDF). Setting this on a project whose layered BSDF is off stores
        /// the value and changes nothing on screen.
        /// </summary>
        public float CoatWeight
        {
            get => aver_pbr_get_coat_weight(Handle);
            set => aver_pbr_set_coat_weight(Handle, value);
        }

        /// <summary>
        /// [0,1] the coat film's own roughness, independent of the base's. Car paint is near 0;
        /// a satin lacquer is higher.
        /// </summary>
        public float CoatRoughness
        {
            get => aver_pbr_get_coat_roughness(Handle);
            set => aver_pbr_set_coat_roughness(Handle, value);
        }

        /// <summary>
        /// Normal-incidence reflectance of the coat film. 0.04 is IOR 1.5 — ordinary lacquer — and is
        /// the default. Distinct from <see cref="Reflectance"/>, which is the BASE material's F0.
        /// </summary>
        public float CoatF0
        {
            get => aver_pbr_get_coat_f0(Handle);
            set => aver_pbr_set_coat_f0(Handle, value);
        }

        /// <summary>
        /// [0,1] thickness PROXY that widens the back-scatter lobe — light entering the far side of
        /// the mesh and travelling toward the eye, which is what makes a leaf or an ear light up when
        /// the sun is behind it. Not a BSSRDF: no transport across the mesh, no per-texel thickness,
        /// no wavelength dependence. The transmitted light is tinted by <see cref="BaseColorFactor"/>;
        /// there is no separate scatter colour to set.
        /// </summary>
        public float SubsurfaceRadius
        {
            get => aver_pbr_get_subsurface_radius(Handle);
            set => aver_pbr_set_subsurface_radius(Handle, value);
        }

        /// <summary>
        /// sRGB tint the scattered light takes INSIDE the material, authored like
        /// <see cref="BaseColorFactor"/> and multiplied onto the diffuse albedo for the subsurface
        /// terms. White (the default) scatters in the surface's own colour; skin wants a deep red,
        /// leaves a yellow-green, wax an orange. Meaningless while <see cref="SubsurfaceWeight"/> is 0.
        /// </summary>
        public (float R, float G, float B) SubsurfaceColor
        {
            get
            {
                var v = new float[3];
                return aver_pbr_get_subsurface_color(Handle, v) != 0 ? (v[0], v[1], v[2]) : (0, 0, 0);
            }
            set => aver_pbr_set_subsurface_color(Handle, value.R, value.G, value.B);
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
