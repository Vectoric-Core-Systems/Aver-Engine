// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The resource types the 3D node stubs hold: meshes, materials and collision shapes. Values only; F2b gives them loaders.

namespace Aver.Fabric;

/// <summary>A mesh resource (Godot's Mesh). <see cref="Resource.ResourcePath"/> names the Aver mesh asset ("res://x.ocmesh" or a content path).</summary>
public class Mesh : Resource
{
    public enum PrimitiveType { Points = 0, Lines = 1, LineStrip = 2, Triangles = 3, TriangleStrip = 4 }

    /// <summary>The slots of the arrays an ArrayMesh surface is made from (RenderingServer.ArrayType).</summary>
    public enum ArrayType { Vertex = 0, Normal = 1, Tangent = 2, Color = 3, TexUV = 4, TexUV2 = 5, Custom0 = 6, Custom1 = 7, Custom2 = 8, Custom3 = 9, Bones = 10, Weights = 11, Index = 12, Max = 13 }

    /// <summary>Surface format flags (RenderingServer.ArrayFormat). Accepted and not needed: the arrays say what a surface has.</summary>
    [Flags]
    public enum ArrayFormat : long
    {
        FormatVertex = 1, FormatNormal = 2, FormatTangent = 4, FormatColor = 8, FormatTexUV = 16, FormatTexUV2 = 32, FormatBones = 1024, FormatWeights = 2048, FormatIndex = 4096,
        FlagUse2DVertices = 1L << 25, FlagUseDynamicUpdate = 1L << 26, FlagUse8BoneWeights = 1L << 27, FlagCompressAttributes = 1L << 29,
    }

    public virtual int GetSurfaceCount() => 0;
    public virtual Material? SurfaceGetMaterial(int surfIdx) => null;
    public virtual void SurfaceSetMaterial(int surfIdx, Material? material) { }
    public virtual Aabb GetAabb() => default;

    /// <summary>The asset path an entity draws this mesh by when the engine holds it as a runtime mesh ("@rtmesh/..."), else null (a file mesh is named by
    /// <see cref="Resource.ResourcePath"/>).</summary>
    internal virtual string? EngineAssetPath => null;
}

/// <summary>A box mesh (Godot's BoxMesh), size in metres.</summary>
public class BoxMesh : Mesh
{
    public Vector3 Size { get; set; } = Vector3.One;
}

public class SphereMesh : Mesh
{
    public float Radius { get; set; } = 0.5f;
    public float Height { get; set; } = 1f;
}

public class Material : Resource
{
    public int RenderPriority { get; set; }
}

public class BaseMaterial3D : Material
{
    public Color AlbedoColor { get; set; } = new(1, 1, 1, 1);
    public float Metallic { get; set; }
    public float Roughness { get; set; } = 1f;
    /// <summary>Multiplies the albedo and opacity by the mesh's vertex colour. The converter bakes it into the .ocmat <c>vertexcolor</c> flag
    /// (docs/plans/fabric/G4_RENDER.md R1); the engine applies it in the raster scene pass for meshes that carry a colour stream.</summary>
    public bool VertexColorUseAsAlbedo { get; set; }
    /// <summary>Godot converts an sRGB vertex colour to linear first; the engine multiplies the colour as stored (linear).</summary>
    public bool VertexColorIsSrgb { get; set; }
    /// <summary>The albedo map. Stored only: the converter bakes a <see cref="ViewportTexture"/> into the .ocmat as <c>@rt:viewport.&lt;key&gt;</c>, which the
    /// engine resolves to the view's texture (G4_RENDER.md R5); assigning a texture here at run time does not change the material.</summary>
    public Texture2D? AlbedoTexture { get; set; }
    public Texture2D? EmissionTexture { get; set; }

    // ---- G4 R6: BaseMaterial3D features, under Godot's property names. The converter bakes them into the .ocmat FX record (see FxRecord);
    // the engine draws the baked material, so a value changed at run time is stored and not pushed (like AlbedoTexture). Distances are metres, as in Godot.
    public enum TransparencyEnum { Disabled = 0, Alpha = 1, AlphaScissor = 2, AlphaHash = 3, AlphaDepthPrePass = 4 }
    public enum ShadingModeEnum { Unshaded = 0, PerPixel = 1, PerVertex = 2 }
    public enum BlendModeEnum { Mix = 0, Add = 1, Sub = 2, Mul = 3, PremultAlpha = 4 }
    public enum CullModeEnum { Back = 0, Front = 1, Disabled = 2 }
    public enum DepthDrawModeEnum { OpaqueOnly = 0, Always = 1, Disabled = 2 }
    public enum BillboardModeEnum { Disabled = 0, Enabled = 1, FixedY = 2, Particles = 3 }
    public enum DiffuseModeEnum { Burley = 0, Lambert = 1, LambertWrap = 2, Toon = 3 }
    public enum SpecularModeEnum { SchlickGgx = 0, Toon = 1, Disabled = 2 }
    public enum DistanceFadeModeEnum { Disabled = 0, PixelAlpha = 1, PixelDither = 2, ObjectDither = 3 }
    public enum DetailUVEnum { DetailUv1 = 0, DetailUv2 = 1 }
    public enum EmissionOperatorEnum { Add = 0, Multiply = 1 }
    public enum AlphaAntiAliasingEnum { Off = 0, AlphaToCoverage = 1, AlphaToCoverageAndToOne = 2 }

    // UV transforms and triplanar
    public Vector3 Uv1Scale { get; set; } = Vector3.One;
    public Vector3 Uv1Offset { get; set; } = Vector3.Zero;
    public Vector3 Uv2Scale { get; set; } = Vector3.One;
    public Vector3 Uv2Offset { get; set; } = Vector3.Zero;
    public bool Uv1Triplanar { get; set; }
    public float Uv1TriplanarSharpness { get; set; } = 1f;
    public bool Uv1WorldTriplanar { get; set; }
    public bool Uv2Triplanar { get; set; }
    public float Uv2TriplanarSharpness { get; set; } = 1f;
    public bool Uv2WorldTriplanar { get; set; }

    // Pipeline state
    public TransparencyEnum Transparency { get; set; } = TransparencyEnum.Disabled;
    public float AlphaScissorThreshold { get; set; } = 0.5f;
    public float AlphaHashScale { get; set; } = 1f;
    public AlphaAntiAliasingEnum AlphaAntialiasingMode { get; set; } = AlphaAntiAliasingEnum.Off;
    public float AlphaAntialiasingEdge { get; set; } = 0.3f;
    public BlendModeEnum BlendMode { get; set; } = BlendModeEnum.Mix;
    public CullModeEnum CullMode { get; set; } = CullModeEnum.Back;
    public DepthDrawModeEnum DepthDrawMode { get; set; } = DepthDrawModeEnum.OpaqueOnly;
    public bool NoDepthTest { get; set; }
    public ShadingModeEnum ShadingMode { get; set; } = ShadingModeEnum.PerPixel;
    public bool ShadowToOpacity { get; set; }
    public bool DisableReceiveShadows { get; set; }
    public bool DisableAmbientLight { get; set; }
    public bool DisableFog { get; set; }

    // Vertex stage and fades
    public BillboardModeEnum BillboardMode { get; set; } = BillboardModeEnum.Disabled;
    public bool BillboardKeepScale { get; set; }
    public bool FixedSize { get; set; }
    public bool Grow { get; set; }
    public float GrowAmount { get; set; }
    public int ParticlesAnimHFrames { get; set; } = 1;
    public int ParticlesAnimVFrames { get; set; } = 1;
    public bool ParticlesAnimLoop { get; set; }
    public DistanceFadeModeEnum DistanceFadeMode { get; set; } = DistanceFadeModeEnum.Disabled;
    public float DistanceFadeMinDistance { get; set; }
    public float DistanceFadeMaxDistance { get; set; } = 10f;
    public bool ProximityFadeEnabled { get; set; }
    public float ProximityFadeDistance { get; set; } = 1f;

    // Surface
    public bool RimEnabled { get; set; }
    public float Rim { get; set; } = 1f;
    public float RimTint { get; set; } = 0.5f;
    public bool ClearcoatEnabled { get; set; }
    public float Clearcoat { get; set; } = 1f;
    public float ClearcoatRoughness { get; set; } = 0.5f;
    public bool AnisotropyEnabled { get; set; }
    public float Anisotropy { get; set; }
    public bool HeightmapEnabled { get; set; }
    public float HeightmapScale { get; set; } = 5f;
    public bool HeightmapDeepParallax { get; set; }
    public int HeightmapMinLayers { get; set; } = 8;
    public int HeightmapMaxLayers { get; set; } = 32;
    public bool HeightmapFlipTangent { get; set; }
    public bool HeightmapFlipBinormal { get; set; }
    public bool HeightmapFlipTexture { get; set; }
    public Texture2D? HeightmapTexture { get; set; }
    public bool SubsurfScatterEnabled { get; set; }
    public float SubsurfScatterStrength { get; set; }
    public bool BacklightEnabled { get; set; }
    public Color Backlight { get; set; } = new(0, 0, 0, 1);
    public bool RefractionEnabled { get; set; }
    public float RefractionScale { get; set; } = 0.05f;
    public bool DetailEnabled { get; set; }
    public BlendModeEnum DetailBlendMode { get; set; } = BlendModeEnum.Mix;
    public DetailUVEnum DetailUvLayer { get; set; } = DetailUVEnum.DetailUv1;
    public Texture2D? DetailAlbedo { get; set; }
    public Texture2D? DetailNormal { get; set; }
    public Texture2D? DetailMask { get; set; }
    public EmissionOperatorEnum EmissionOperator { get; set; } = EmissionOperatorEnum.Add;
    public bool EmissionOnUv2 { get; set; }
    public bool AoOnUv2 { get; set; }
    public float AoLightAffect { get; set; }
    public float MetallicSpecular { get; set; } = 0.5f;
    public DiffuseModeEnum DiffuseMode { get; set; } = DiffuseModeEnum.Burley;
    public SpecularModeEnum SpecularMode { get; set; } = SpecularModeEnum.SchlickGgx;

    private static string F(double v) => v.ToString("0.######", System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>The pairs of the .ocmat <c>FX</c> record this material's properties bake to: the grammar the converter writes and
    /// <c>aver::fmt::parseOcmatFx</c> reads (docs/plans/fabric/G4_RENDER.md R6). Only what differs from Godot's defaults is listed, plus the cull mode.</summary>
    public string FxRecord()
    {
        var p = new System.Collections.Generic.List<string>();
        p.Add(CullMode == CullModeEnum.Front ? "cull=front" : CullMode == CullModeEnum.Disabled ? "cull=none" : "cull=back");
        if (Uv1Scale.X != 1 || Uv1Scale.Y != 1) p.Add($"uv1scale={F(Uv1Scale.X)},{F(Uv1Scale.Y)}");
        if (Uv1Offset.X != 0 || Uv1Offset.Y != 0) p.Add($"uv1offset={F(Uv1Offset.X)},{F(Uv1Offset.Y)}");
        if (Uv2Scale.X != 1 || Uv2Scale.Y != 1) p.Add($"uv2scale={F(Uv2Scale.X)},{F(Uv2Scale.Y)}");
        if (Uv2Offset.X != 0 || Uv2Offset.Y != 0) p.Add($"uv2offset={F(Uv2Offset.X)},{F(Uv2Offset.Y)}");
        if (Uv1Triplanar) { p.Add("uv1tri=1"); if (Uv1WorldTriplanar) p.Add("uv1world=1"); if (Uv1TriplanarSharpness != 1) p.Add($"uv1sharp={F(Uv1TriplanarSharpness)}"); }
        if (Transparency == TransparencyEnum.AlphaHash) { p.Add("alphaext=hash"); if (AlphaHashScale != 1) p.Add($"hashscale={F(AlphaHashScale)}"); }
        if (Transparency == TransparencyEnum.AlphaDepthPrePass) p.Add("depthdraw=always");
        if (AlphaAntialiasingMode != AlphaAntiAliasingEnum.Off && (Transparency == TransparencyEnum.AlphaScissor || Transparency == TransparencyEnum.AlphaHash)) p.Add("alphaext=aa");
        if (BlendMode != BlendModeEnum.Mix) p.Add("blend=" + new[] { "mix", "add", "sub", "mul", "premult" }[(int)BlendMode]);
        if (DepthDrawMode == DepthDrawModeEnum.Always) p.Add("depthdraw=always");
        if (DepthDrawMode == DepthDrawModeEnum.Disabled) p.Add("depthdraw=never");
        if (NoDepthTest) p.Add("nodepthtest=1");
        if (RenderPriority != 0) p.Add($"priority={RenderPriority}");
        if (ShadingMode == ShadingModeEnum.Unshaded) p.Add("unshaded=1");
        if (DisableReceiveShadows) p.Add("noshadows=1");
        if (DisableAmbientLight) p.Add("noambient=1");
        if (BillboardMode != BillboardModeEnum.Disabled)
        {
            p.Add("billboard=" + (BillboardMode == BillboardModeEnum.Enabled ? "on" : BillboardMode == BillboardModeEnum.FixedY ? "y" : "particles"));
            if (BillboardKeepScale) p.Add("keepscale=1");
            if (BillboardMode == BillboardModeEnum.Particles) p.Add($"frames={ParticlesAnimHFrames},{ParticlesAnimVFrames}");
        }
        if (FixedSize) p.Add("fixedsize=1");
        if (Grow) { p.Add("grow=1"); p.Add($"growcm={F(GrowAmount * 100.0)}"); }
        if (DistanceFadeMode != DistanceFadeModeEnum.Disabled)
        {
            p.Add("fade=" + (DistanceFadeMode == DistanceFadeModeEnum.PixelAlpha ? "alpha" : DistanceFadeMode == DistanceFadeModeEnum.PixelDither ? "dither" : "object"));
            p.Add($"fademin={F(DistanceFadeMinDistance * 100.0)}");
            p.Add($"fademax={F(DistanceFadeMaxDistance * 100.0)}");
        }
        if (RimEnabled) { p.Add("rim=1"); p.Add($"rimamount={F(Rim)}"); p.Add($"rimtint={F(RimTint)}"); }
        if (AnisotropyEnabled) { p.Add("aniso=1"); p.Add($"anisoamount={F(Anisotropy)}"); }
        if (HeightmapEnabled)
        {
            p.Add("height=1"); p.Add($"heightscale={F(HeightmapScale)}");
            if (HeightmapDeepParallax) { p.Add("deepparallax=1"); p.Add($"layers={HeightmapMinLayers},{HeightmapMaxLayers}"); }
            if (HeightmapFlipTangent) p.Add("flipt=1");
            if (HeightmapFlipBinormal) p.Add("flipb=1");
            if (HeightmapFlipTexture) p.Add("invertheight=1");
        }
        if (BacklightEnabled) { p.Add("backlight=1"); p.Add($"backlightcolor={F(Backlight.R)},{F(Backlight.G)},{F(Backlight.B)}"); }
        if (DetailEnabled)
        {
            p.Add("detail=1");
            if (DetailBlendMode != BlendModeEnum.Mix) p.Add($"detailblend={System.Math.Min((int)DetailBlendMode, 3)}");
            if (DetailUvLayer == DetailUVEnum.DetailUv2) p.Add("detailuv=2");
        }
        if (EmissionOperator == EmissionOperatorEnum.Multiply) p.Add("emitmul=1");
        if (EmissionOnUv2) p.Add("emituv2=1");
        if (AoOnUv2) p.Add("aouv2=1");
        if (AoLightAffect != 0) p.Add($"aoaffect={F(AoLightAffect)}");
        if (SpecularMode != SpecularModeEnum.SchlickGgx) p.Add("spec=" + (SpecularMode == SpecularModeEnum.Toon ? "toon" : "off"));
        if (DiffuseMode != DiffuseModeEnum.Burley) p.Add("diffuse=" + (DiffuseMode == DiffuseModeEnum.Lambert ? "lambert" : DiffuseMode == DiffuseModeEnum.LambertWrap ? "wrap" : "toon"));
        return string.Join(' ', p);
    }

    /// <summary>The F0 of the dielectric base the converter writes: 0.08 * metallic_specular (Godot's rule).</summary>
    public float Reflectance => MetallicSpecular * 0.08f;
}

public class StandardMaterial3D : BaseMaterial3D
{
}

/// <summary>Godot's ORMMaterial3D: a StandardMaterial3D whose occlusion, roughness and metallic come from one packed texture (R, G, B), the layout the
/// .ocmat packs. The converter maps both classes the same way.</summary>
public class ORMMaterial3D : BaseMaterial3D
{
    public Texture2D? OrmTexture { get; set; }
}

public class Texture2D : Resource
{
    /// <summary>The size in pixels. A file texture's size is not read at run time (the engine loads it), so this is zero for one; a
    /// <see cref="ViewportTexture"/> reports the size its view is drawn at.</summary>
    public virtual Vector2 GetSize() => Vector2.Zero;

    public int GetWidth() => (int)GetSize().X;

    public int GetHeight() => (int)GetSize().Y;
}

/// <summary>A collision shape resource (Godot's Shape3D). Sizes are in metres.</summary>
public class Shape3D : Resource
{
    public float Margin { get; set; } = 0.04f;
}

public class BoxShape3D : Shape3D
{
    /// <summary>The full size of the box (not the half extents).</summary>
    public Vector3 Size { get; set; } = Vector3.One;
}

public class SphereShape3D : Shape3D
{
    public float Radius { get; set; } = 0.5f;
}

public class CapsuleShape3D : Shape3D
{
    public float Radius { get; set; } = 0.5f;
    /// <summary>The whole height, including both caps.</summary>
    public float Height { get; set; } = 2f;
}

public class CylinderShape3D : Shape3D
{
    public float Radius { get; set; } = 0.5f;
    public float Height { get; set; } = 2f;
}

public class WorldBoundaryShape3D : Shape3D
{
    public Plane Plane { get; set; } = new(0, 1, 0, 0);
}

public class ConvexPolygonShape3D : Shape3D
{
    public Vector3[] Points { get; set; } = System.Array.Empty<Vector3>();
}

public class ConcavePolygonShape3D : Shape3D
{
    public Vector3[] Data { get; set; } = System.Array.Empty<Vector3>();
}
