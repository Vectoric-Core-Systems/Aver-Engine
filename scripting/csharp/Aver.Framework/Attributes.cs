// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The gameplay attributes: class registration, GameMode wiring, editable fields and model slots.

namespace Aver.Framework;

/// <summary>Registers the type it decorates as a gameplay class and names its parent class.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverClassAttribute : Attribute
{
    /// <summary>Registers the class under <paramref name="name"/>.</summary>
    public AverClassAttribute(string name) => Name = name;

    /// <summary>The registry name, which need not match the C# identifier.</summary>
    public string Name { get; }

    /// <summary>The parent class name, resolved at seal.</summary>
    public string Parent { get; init; } = "Actor";
}

/// <summary>Registers a GameMode class and names, by string, the pawn and controller it hands out.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverGameModeAttribute : Attribute
{
    /// <summary>Registers the GameMode under <paramref name="name"/>.</summary>
    public AverGameModeAttribute(string name) => Name = name;

    /// <summary>The registry name.</summary>
    public string Name { get; }
    /// <summary>Class name of the pawn every joining player is given.</summary>
    public string DefaultPawnClass { get; init; } = "Pawn";
    /// <summary>Class name of the controller that possesses that pawn.</summary>
    public string PlayerControllerClass { get; init; } = "PlayerController";
}

/// <summary>Marks a field as a class default with a per-instance override, stored natively and serialised.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class EditableAttribute : Attribute
{
    /// <summary>Lower clamp in the inspector and on deserialisation.</summary>
    public float Min { get; init; } = float.NegativeInfinity;
    /// <summary>Upper clamp in the inspector and on deserialisation.</summary>
    public float Max { get; init; } = float.PositiveInfinity;
}

/// <summary>Marks a property in the generated region as one model slot the viewport gizmo drives.</summary>
[AttributeUsage(AttributeTargets.Property)]
public sealed class ModelAttribute : Attribute
{
}
