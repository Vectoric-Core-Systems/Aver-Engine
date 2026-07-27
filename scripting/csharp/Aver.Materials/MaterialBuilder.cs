using System.Globalization;
using System.Text;

namespace Aver.Materials;

/// <summary>
/// Declares one material, and emits the <c>.ocmat</c> the engine reads.
/// </summary>
/// <remarks>
/// <para>
/// The C# class in <c>Content/Materials</c> is the SOURCE; the <c>.ocmat</c> under <c>Binaries</c> is
/// the build output, and the engine only ever reads the latter. Editing the generated file is
/// editing a build artefact — it is overwritten on the next compile, and the header it carries says so.
/// </para>
/// <para>
/// This writes the format rather than calling into the engine to write it, which means the grammar
/// has TWO writers: this one, and <c>modules/formats/src/OcMat.cpp</c>. That is a real cost and it is
/// paid deliberately — the alternative is a native ABI on the build path, which would make baking a
/// material require a loaded engine. The mitigation is that <c>MaterialCompilerTest</c> writes from
/// here and reads with the C++ reader, so the two cannot drift without a test going red.
/// </para>
/// </remarks>
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

    private readonly Dictionary<Slot, string> _textures = new();

    internal MaterialBuilder(string name) => _name = name;

    /// <summary>A line of explanation, written into the output above the parameters.</summary>
    /// <remarks>
    /// Carried through on purpose. The generated file is what somebody debugging a surface actually
    /// opens, and a bare table of numbers with the reasoning left behind in a .cs file is exactly how
    /// a value ends up mysterious.
    /// </remarks>
    public MaterialBuilder Comment(string text) { _comments.Add(text); return this; }

    /// <summary>The shading model.</summary>
    public MaterialBuilder Shader(Shading s) { _shader = s; return this; }

    /// <summary>How the surface composites. <paramref name="cutoff"/> applies to <see cref="Blend.Masked"/> only.</summary>
    public MaterialBuilder Blending(Blend b, float cutoff = 0.5f) { _blend = b; _cutoff = cutoff; return this; }

    /// <summary>Which faces are drawn. <see cref="Cull.None"/> makes the material two-sided.</summary>
    public MaterialBuilder Culling(Cull c) { _cull = c; return this; }

    /// <summary>Whether the surface casts shadows. On by default.</summary>
    public MaterialBuilder CastShadow(bool on) { _castShadow = on; return this; }

    /// <summary>
    /// Project texture coordinates from world space rather than the mesh's UVs, at
    /// <see cref="Tiling"/> centimetres per tile. What untextured blockout geometry wants.
    /// </summary>
    public MaterialBuilder WorldUv(bool on) { _worldUv = on; return this; }

    /// <summary>Base colour multiplier, linear 0..1.</summary>
    public MaterialBuilder BaseColor(float r, float g, float b, float a = 1f)
    { _baseColor = new[] { r, g, b, a }; return this; }

    /// <summary>Metallic multiplier. 1 means "whatever the metalRough texture says".</summary>
    public MaterialBuilder Metallic(float v) { _metallic = v; return this; }

    /// <summary>Roughness multiplier. 1 means "whatever the metalRough texture says".</summary>
    public MaterialBuilder Roughness(float v) { _roughness = v; return this; }

    /// <summary>Emission, linear. Not a colour multiplier — it is added, so it lights nothing but itself.</summary>
    public MaterialBuilder Emissive(float r, float g, float b) { _emissive = new[] { r, g, b }; return this; }

    /// <summary>How strongly the normal map is applied. Above 1 exaggerates it.</summary>
    public MaterialBuilder NormalScale(float v) { _normalScale = v; return this; }

    /// <summary>How strongly baked occlusion darkens ambient light.</summary>
    public MaterialBuilder OcclusionStrength(float v) { _occlusionStrength = v; return this; }

    /// <summary>Dielectric reflectance at normal incidence. 0.04 is almost every non-metal.</summary>
    public MaterialBuilder Reflectance(float v) { _reflectance = v; return this; }

    /// <summary>Reflectance at grazing incidence. Below 1 tames the rim on rough surfaces.</summary>
    public MaterialBuilder F90(float v) { _f90 = v; return this; }

    /// <summary>World centimetres per texture tile. Only meaningful with <see cref="WorldUv"/> on.</summary>
    public MaterialBuilder Tiling(float centimetres) { _uvTiling = centimetres; return this; }

    /// <summary>
    /// Bind a texture. The path is relative to the project's content root, using forward slashes.
    /// </summary>
    /// <remarks>
    /// There is no colour-space argument, and that is deliberate: the space is a property of the SLOT
    /// (base colour and emissive are sRGB, normal is a normal map, the rest are linear), so letting a
    /// caller pass one would only let a caller pass the wrong one.
    /// </remarks>
    public MaterialBuilder Texture(Slot slot, string contentRelativePath)
    { _textures[slot] = contentRelativePath.Replace('\\', '/'); return this; }

    // ---- emission ----

    private static string Num(float v)
    {
        // Round-trippable and culture-invariant. "R" so a value survives write-read-write unchanged,
        // and InvariantCulture so a machine set to a comma decimal separator does not emit
        // "0,04" — which the reader would take as two tokens and get silently wrong.
        return v.ToString("R", CultureInfo.InvariantCulture);
    }

    private static string SlotName(Slot s) => s switch
    {
        Slot.BaseColor => "baseColor",
        Slot.MetalRough => "metalRough",
        Slot.Normal => "normal",
        Slot.Occlusion => "occlusion",
        Slot.Emissive => "emissive",
        _ => "?",
    };

    // The colour space the reader expects for each slot. It VALIDATES this token rather than reading
    // it, so a wrong one is a warning and a right one is silence — but writing the wrong one would
    // still be writing something untrue into a file somebody reads.
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
        // Named as generated, and by WHAT, so somebody who opens it and starts editing finds out
        // here rather than after losing the edit on the next build.
        s.Append("# GENERATED by the Aver material compiler from ").Append(sourceFile).Append(".\n");
        s.Append("# Edits here are overwritten. Change the C# source instead.\n");
        foreach (string c in _comments) s.Append("# ").Append(c).Append('\n');

        s.Append("NAME ").Append(_name).Append('\n');
        s.Append("SHADER ").Append(_shader == Shading.Standard ? "standard" : "standard").Append('\n');

        s.Append("BLEND ");
        s.Append(_blend switch
        {
            Blend.Masked => "masked " + Num(_cutoff),
            Blend.Translucent => "translucent",
            Blend.Additive => "additive",
            _ => "opaque",
        });
        s.Append('\n');

        // twoSided is what the engine's own writer treats as authoritative, and CULL is derived from
        // it. Mirrored here so a file from this compiler and one from the editor cannot disagree
        // about which of the two lines wins.
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
        // Written unconditionally, matching the engine's writer: the value is part of the material
        // whether or not world UVs are on, and omitting it would reset a surface's tiling every time
        // somebody toggled the mode off and back on.
        s.Append("PARAM uvTiling ").Append(Num(_uvTiling)).Append('\n');

        if (_textures.Count > 0)
        {
            s.Append('\n');
            // Slot order, not insertion order, so the same declaration always produces the same
            // bytes. A generated file that reorders itself between builds is a diff nobody can read.
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
    /// <remarks>
    /// Static rather than virtual, matching how <c>[AverClass]</c> types declare themselves: a
    /// material is never instantiated, so there is no instance for a virtual call to dispatch on.
    /// </remarks>
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
