namespace Aver.Materials;

/// <summary>Marks a class as a material source. The name is what a mesh binds to.</summary>
/// <remarks>
/// The name is given here rather than taken from the class name, for the same reason
/// <c>[AverClass]</c> does it: a level references a material by name, and renaming a C# class should
/// not silently unbind every mesh that used it.
/// </remarks>
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

/// <summary>
/// A texture slot. The COLOUR SPACE is not listed here on purpose: it is a property of the slot, not
/// of the binding — base colour and emissive are always sRGB, normal is always a normal map, the
/// rest are always linear. Letting a caller choose would let a caller choose wrong.
/// </summary>
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

/// <summary>
/// The base class a material source derives from. It carries nothing — the declaration is entirely in
/// the <c>Configure</c> method — and exists so a material is findable by type rather than only by
/// attribute, and so an editor can offer "new material" against something concrete.
/// </summary>
/// <remarks>
/// A material class is never instantiated and never ticks. It is a description that the compiler runs
/// once, at build time, to produce an <c>.ocmat</c>. Nothing about it exists at runtime.
/// </remarks>
public abstract class Material
{
    /// <summary>
    /// Declare the surface. Called ONCE by the material compiler, at build time, never at runtime.
    /// </summary>
    /// <remarks>
    /// Declared here for documentation only: the compiler finds it by reflection, because a static
    /// method cannot be virtual. A material class without one produces nothing and is reported.
    /// </remarks>
    public static void Configure(MaterialBuilder b) { }
}
