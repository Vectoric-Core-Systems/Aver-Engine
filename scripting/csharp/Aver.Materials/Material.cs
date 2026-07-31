// The material authoring surface: the marker attribute, the surface enums, and the base class.
namespace Aver.Materials;

/// <summary>Marks a class as a material source. The name is what a mesh binds to.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverMaterialAttribute : Attribute
{
    /// <summary>The bound name, e.g. <c>M_Crate</c>. This is what ends up in <c>NAME</c>.</summary>
    public string Name { get; }

    /// <summary>Marks a class as a material source under the given bound name.</summary>
    public AverMaterialAttribute(string name) => Name = name;
}

/// <summary>Which shading model. Only <see cref="Standard"/> exists so far.</summary>
public enum Shading
{
    /// <summary>The metallic-roughness BRDF every surface uses today.</summary>
    Standard,
}

/// <summary>How the surface composites.</summary>
public enum Blend
{
    /// <summary>No blending. What almost everything is.</summary>
    Opaque,
    /// <summary>Alpha-tested against a cutoff. Foliage, chain-link, decals with hard edges.</summary>
    Masked,
    /// <summary>Alpha blended. Glass, water.</summary>
    Translucent,
    /// <summary>Adds. Fire, light shafts, anything that only ever brightens.</summary>
    Additive,
}

/// <summary>Which faces are drawn.</summary>
public enum Cull
{
    /// <summary>Back faces are discarded. The default and what solid geometry wants.</summary>
    Back,
    /// <summary>Front faces are discarded. Rare, and usually a modelling mistake made deliberate.</summary>
    Front,
    /// <summary>Nothing is discarded. Implies two-sided.</summary>
    None,
}

/// <summary>A texture slot. The colour space is fixed per slot, not chosen at the binding.</summary>
public enum Slot
{
    /// <summary>Albedo. sRGB.</summary>
    BaseColor,
    /// <summary>Metallic in blue, roughness in green — the glTF packing. Linear.</summary>
    MetalRough,
    /// <summary>Tangent-space normals.</summary>
    Normal,
    /// <summary>Ambient occlusion, usually sharing the metalRough texture's red channel. Linear.</summary>
    Occlusion,
    /// <summary>Emission. sRGB.</summary>
    Emissive,
}

/// <summary>The base class a material source derives from. Carries no state; never instantiated.</summary>
public abstract class Material
{
    /// <summary>Declares the surface. Called once by the material compiler, at build time.</summary>
    public static void Configure(MaterialBuilder b) { }
}
