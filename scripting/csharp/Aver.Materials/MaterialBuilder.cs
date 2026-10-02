// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Declares one material and emits the .ocmat text the engine reads.
using System;
using System.Globalization;
using System.Text;

namespace Aver.Materials;

/// <summary>Declares one material, and emits the <c>.ocmat</c> the engine reads.</summary>
/// <remarks>The grammar has a second writer in <c>modules/formats/src/OcMat.cpp</c>; the two must agree.</remarks>
public sealed class MaterialBuilder
{
    private readonly string _name;
    private readonly List<string> _comments = new();

    private Shading _shader = Shading.Standard;
    private Blend _blend = Blend.Opaque;
    private float _cutoff = 0.5f;
    private Cull _cull = Cull.Back;
    private bool _castShadow = true;
    private bool _worldUv;

    private float[] _baseColor = { 1f, 1f, 1f, 1f };
    private float _metallic = 1f;
    private float _roughness = 1f;
    private float[] _emissive = { 0f, 0f, 0f };
    private float _normalScale = 1f;
    private float _occlusionStrength = 1f;
    private float _reflectance = 0.04f;
    private float _f90 = 1f;
    private float _uvTiling = 100f;
    // 1.5 and 0 are MaterialDesc's own defaults, so a builder that never touches them emits nothing
    // and the .ocmat text is unchanged.
    private float _ior = 1.5f;
    private float _transmission;
    private float _subsurfaceWeight;
    private float _subsurfaceRadius;
    private float[] _subsurfaceColor = { 1f, 1f, 1f };   // MaterialDesc's own default: white, scatter in the surface's own colour
    private float _coatWeight;
    private float _coatRoughness;
    private float _coatF0 = 0.04f;   // the field default, so an unset coat emits nothing
    private float _lightIntensity;   // 0 = not a light, MaterialDesc's own default

    private readonly Dictionary<Slot, string> _textures = new();

    /// <summary>Starts a material bound to the given name.</summary>
    internal MaterialBuilder(string name) => _name = name;

    /// <summary>A line of explanation, written into the output above the parameters.</summary>
    public MaterialBuilder Comment(string text) { _comments.Add(text); return this; }

    /// <summary>The shading model.</summary>
    public MaterialBuilder Shader(Shading s) { _shader = s; return this; }

    /// <summary>How the surface composites. <paramref name="cutoff"/> applies to <see cref="Blend.Masked"/> only.</summary>
    public MaterialBuilder Blending(Blend b, float cutoff = 0.5f) { _blend = b; _cutoff = cutoff; return this; }

    /// <summary>Which faces are drawn. <see cref="Cull.None"/> makes the material two-sided.</summary>
    public MaterialBuilder Culling(Cull c) { _cull = c; return this; }

    /// <summary>Whether the surface casts shadows. On by default.</summary>
    public MaterialBuilder CastShadow(bool on) { _castShadow = on; return this; }

    /// <summary>Project texture coordinates from world space at <see cref="Tiling"/> centimetres per tile.</summary>
    public MaterialBuilder WorldUv(bool on) { _worldUv = on; return this; }

    /// <summary>Base colour multiplier, 0..1: rgb sRGB-encoded like a colour picked in the editor (the
    /// engine decodes it with pow 2.2), alpha linear coverage.</summary>
    public MaterialBuilder BaseColor(float r, float g, float b, float a = 1f)
    { _baseColor = new[] { r, g, b, a }; return this; }

    /// <summary>Metallic multiplier. 1 means "whatever the metalRough texture says".</summary>
    public MaterialBuilder Metallic(float v) { _metallic = v; return this; }

    /// <summary>Roughness multiplier. 1 means "whatever the metalRough texture says".</summary>
    public MaterialBuilder Roughness(float v) { _roughness = v; return this; }

    /// <summary>Emission, linear. Added rather than multiplied, so it lights nothing but itself.</summary>
    public MaterialBuilder Emissive(float r, float g, float b) { _emissive = new[] { r, g, b }; return this; }

    /// <summary>How strongly the normal map is applied. Above 1 exaggerates it.</summary>
    public MaterialBuilder NormalScale(float v) { _normalScale = v; return this; }

    /// <summary>How strongly baked occlusion darkens ambient light.</summary>
    public MaterialBuilder OcclusionStrength(float v) { _occlusionStrength = v; return this; }

    /// <summary>Dielectric reflectance at normal incidence. 0.04 is almost every non-metal.</summary>
    public MaterialBuilder Reflectance(float v) { _reflectance = v; return this; }

    /// <summary>Reflectance at grazing incidence. Below 1 tames the rim on rough surfaces.</summary>
    public MaterialBuilder F90(float v) { _f90 = v; return this; }

    /// <summary>
    /// Wrap-diffuse weight, [0,1]: how far light bends past the terminator. 0 is the feature's own
    /// off switch -- not a BSSRDF, no transport across the mesh, just a wider diffuse wrap plus a
    /// view-dependent back-scatter lobe tinted by <see cref="BaseColor"/>. See MaterialDesc for the
    /// full explanation of what this approximates and what it does not.
    /// </summary>
    /// <summary>
    /// Refractive index of the substrate: 1.0 vacuum, ~1.33 water, ~1.5 window glass, ~2.42 diamond.
    /// Sets the critical angle for total internal reflection, and is the quantity
    /// <see cref="Reflectance"/> is physically derived from — setting one without the other can
    /// describe a substance that does not exist.
    /// </summary>
    public MaterialBuilder Ior(float v) { _ior = v; return this; }

    /// <summary>
    /// [0,1] how optically see-through the substrate is. Pulls blended coverage toward
    /// (1 - transmission) before the view-angle Fresnel lifts it back, and scales the diffuse lobe so
    /// a transmissive surface does not also scatter its full base colour back at the viewer.
    /// NOT refraction: light does not bend passing through.
    /// </summary>
    public MaterialBuilder Transmission(float v) { _transmission = v; return this; }

    public MaterialBuilder SubsurfaceWeight(float v) { _subsurfaceWeight = v; return this; }

    /// <summary>
    /// Thickness proxy, [0,1], that widens the back-scatter lobe -- what makes a leaf or an ear light
    /// up when the sun is behind it. Meaningless while <see cref="SubsurfaceWeight"/> is 0.
    /// </summary>
    public MaterialBuilder SubsurfaceRadius(float v) { _subsurfaceRadius = v; return this; }

    /// <summary>
    /// sRGB tint of light scattered inside the material, authored like <see cref="BaseColor"/>. White
    /// (the default) scatters in the surface's own colour; skin wants a deep red, leaves a
    /// yellow-green, wax an orange. Meaningless while <see cref="SubsurfaceWeight"/> is 0.
    /// </summary>
    public MaterialBuilder SubsurfaceColor(float r, float g, float b) { _subsurfaceColor = new[] { r, g, b }; return this; }

    /// <summary>
    /// [0,1] clear coat over the base material -- car paint, varnish, a wet stone. 0 is no coat.
    /// Whether the renderer evaluates it is a project-wide setting (RENDER.LAYEREDBSDF), not this.
    /// </summary>
    public MaterialBuilder CoatWeight(float v) { _coatWeight = v; return this; }

    /// <summary>[0,1] the coat film's own roughness. Meaningless while <see cref="CoatWeight"/> is 0.</summary>
    public MaterialBuilder CoatRoughness(float v) { _coatRoughness = v; return this; }

    /// <summary>Normal-incidence reflectance of the coat film; 0.04 (IOR 1.5) is ordinary lacquer.</summary>
    public MaterialBuilder CoatF0(float v) { _coatF0 = v; return this; }

    /// <summary>
    /// Makes this material a light source when ray tracing is on. A MULTIPLIER on the light a glowing
    /// sphere of this material's <see cref="Emissive"/> and this draw's bounding-sphere size physically
    /// casts: 1 is exactly that, 2 is twice it -- not a brightness by itself. 0 is the default and
    /// means "not a light". Coloured by <see cref="Emissive"/> (white if it is left at zero). Without
    /// ray tracing the material is shaded by its Emissive factor alone.
    /// </summary>
    public MaterialBuilder LightIntensity(float v) { _lightIntensity = v; return this; }

    /// <summary>World centimetres per texture tile. Only meaningful with <see cref="WorldUv"/> on.</summary>
    public MaterialBuilder Tiling(float centimetres) { _uvTiling = centimetres; return this; }

    /// <summary>Binds a texture to a slot. The path is content-root relative, forward slashes.</summary>
    public MaterialBuilder Texture(Slot slot, string contentRelativePath)
    { _textures[slot] = contentRelativePath.Replace('\\', '/'); return this; }

    /// <summary>Formats a float round-trippably and culture-invariantly.</summary>
    private static string Num(float v)
    {
        return v.ToString("R", CultureInfo.InvariantCulture);
    }

    /// <summary>The token a slot is written as in a <c>TEX</c> line.</summary>
    private static string SlotName(Slot s) => s switch
    {
        Slot.BaseColor => "baseColor",
        Slot.MetalRough => "metalRough",
        Slot.Normal => "normal",
        Slot.Occlusion => "occlusion",
        Slot.Emissive => "emissive",
        _ => "?",
    };

    /// <summary>The colour-space token the reader validates each slot against.</summary>
    private static string SlotColourSpace(Slot s) => s switch
    {
        Slot.BaseColor => "sRGB",
        Slot.Emissive => "sRGB",
        Slot.Normal => "normal",
        _ => "linear",
    };

    /// <summary>The <c>.ocmat</c> text this material compiles to.</summary>
    public string Emit(string sourceFile)
    {
        var s = new StringBuilder(1024);
        s.Append("OCMAT 1\n");
        s.Append("# GENERATED by the Aver material compiler from ").Append(sourceFile).Append(".\n");
        s.Append("# Edits here are overwritten. Change the C# source instead.\n");
        foreach (string c in _comments) s.Append("# ").Append(c).Append('\n');

        s.Append("NAME ").Append(_name).Append('\n');
        // BOTH ARMS OF THE TERNARY THIS REPLACES WERE "standard". Harmless while Shading has one
        // member, and a silent data-loss bug the moment it has two: a material authored with a
        // second shading model would emit "standard" and nobody would be told. A switch that throws
        // on an unhandled member turns that into a loud failure at the moment the member is added,
        // which is the only moment anyone can act on it.
        s.Append("SHADER ").Append(_shader switch
        {
            Shading.Standard => "standard",
            _ => throw new NotSupportedException(
                     $"MaterialBuilder cannot write Shading.{_shader} -- this writer and the "
                     + "grammar in modules/formats/src/OcMat.cpp must both learn a new shading "
                     + "model before one can be authored."),
        }).Append('\n');

        s.Append("BLEND ");
        s.Append(_blend switch
        {
            Blend.Masked => "masked " + Num(_cutoff),
            Blend.Translucent => "translucent",
            Blend.Additive => "additive",
            _ => "opaque",
        });
        s.Append('\n');

        // twoSided is authoritative and CULL is derived from it, matching the engine's own writer.
        bool twoSided = _cull == Cull.None;
        s.Append("CULL ").Append(twoSided ? "none" : (_cull == Cull.Front ? "front" : "back")).Append('\n');
        s.Append("FLAGS twosided=").Append(twoSided ? '1' : '0')
         .Append(" castshadow=").Append(_castShadow ? '1' : '0')
         .Append(" worlduv=").Append(_worldUv ? '1' : '0').Append("\n\n");

        s.Append("PARAM baseColorFactor ").Append(Num(_baseColor[0])).Append(' ').Append(Num(_baseColor[1]))
         .Append(' ').Append(Num(_baseColor[2])).Append(' ').Append(Num(_baseColor[3])).Append('\n');
        s.Append("PARAM metallicFactor ").Append(Num(_metallic)).Append('\n');
        s.Append("PARAM roughnessFactor ").Append(Num(_roughness)).Append('\n');
        s.Append("PARAM emissiveFactor ").Append(Num(_emissive[0])).Append(' ').Append(Num(_emissive[1]))
         .Append(' ').Append(Num(_emissive[2])).Append('\n');
        s.Append("PARAM normalScale ").Append(Num(_normalScale)).Append('\n');
        s.Append("PARAM occlusionStrength ").Append(Num(_occlusionStrength)).Append('\n');
        s.Append("PARAM reflectance ").Append(Num(_reflectance)).Append('\n');
        s.Append("PARAM f90 ").Append(Num(_f90)).Append('\n');
        s.Append("PARAM uvTiling ").Append(Num(_uvTiling)).Append('\n');
        // OMITTED WHEN OFF, unlike every PARAM above: subsurfaceWeight 0 is not merely a number but
        // the feature's own off switch (MaterialGpu.cpp's packMaterial sets MaterialFlag_Subsurface
        // exactly when this is > 0), so a material that never asked for the wrap term has nothing
        // meaningful to round-trip. Matches modules/formats/src/OcMat.cpp's writer exactly, including
        // subsurfaceRadius riding along unconditionally once weight is set -- it is meaningless
        // without the weight that gates it, so there is no separate "opt in radius alone" case.
        // OPT-IN, like the subsurface block below and for the same reason: writing "PARAM ior 1.5"
        // into every material in the tree would diff every fixture to record a value that was already
        // the default. Compared against the defaults declared above, not against 0.
        if (_ior != 1.5f)          s.Append("PARAM ior ").Append(Num(_ior)).Append('\n');
        if (_transmission > 0f)    s.Append("PARAM transmission ").Append(Num(_transmission)).Append('\n');
        if (_subsurfaceWeight > 0f)
        {
            s.Append("PARAM subsurfaceWeight ").Append(Num(_subsurfaceWeight)).Append('\n');
            s.Append("PARAM subsurfaceRadius ").Append(Num(_subsurfaceRadius)).Append('\n');
            s.Append("PARAM subsurfaceColor ").Append(Num(_subsurfaceColor[0])).Append(' ')
             .Append(Num(_subsurfaceColor[1])).Append(' ').Append(Num(_subsurfaceColor[2])).Append('\n');
        }

        // Gated on the weight, and emitting all three together, exactly as OcMat.cpp's writer does.
        // THE TWO WRITERS MUST AGREE and nothing checks that they do -- this class's own remarks say
        // so ("The grammar has a second writer in modules/formats/src/OcMat.cpp; the two must",
        // "agree"). An unrecognised PARAM is silently dropped by the parser, so a key emitted here
        // and not handled there produces a material that is quietly missing its coat, with no error
        // anywhere. Adding a PARAM in one place and not the other is the whole failure mode.
        if (_coatWeight > 0f)
        {
            s.Append("PARAM coatWeight ").Append(Num(_coatWeight)).Append('\n');
            s.Append("PARAM coatRoughness ").Append(Num(_coatRoughness)).Append('\n');
            s.Append("PARAM coatF0 ").Append(Num(_coatF0)).Append('\n');
        }

        // OMITTED WHEN OFF, same reasoning as the subsurface/coat blocks above: 0 is not merely a
        // default but the feature's own off switch, so a material that never asked to be a light
        // gets no line at all. Matches modules/formats/src/OcMat.cpp's writer -- see this class's own
        // remarks on why the two must agree.
        if (_lightIntensity > 0f) s.Append("PARAM lightIntensity ").Append(Num(_lightIntensity)).Append('\n');

        if (_textures.Count > 0)
        {
            s.Append('\n');
            // Slot order, not insertion order, so the same declaration always produces the same bytes.
            foreach (Slot slot in Enum.GetValues<Slot>())
            {
                if (!_textures.TryGetValue(slot, out string? path)) continue;
                s.Append("TEX ").Append(SlotName(slot)).Append(" {path:").Append(path)
                 .Append("} uv0 ").Append(SlotColourSpace(slot)).Append('\n');
            }
        }

        return s.ToString();
    }

    /// <summary>Runs a material type's <c>Configure</c> and returns the builder it filled in.</summary>
    public static MaterialBuilder Run(Type type, string boundName)
    {
        var b = new MaterialBuilder(boundName);
        var configure = type.GetMethod("Configure",
            System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static,
            new[] { typeof(MaterialBuilder) });
        if (configure is null)
            throw new InvalidOperationException(
                $"{type.FullName} is marked [AverMaterial] but has no `public static void Configure(MaterialBuilder)`");
        configure.Invoke(null, new object[] { b });
        return b;
    }
}
