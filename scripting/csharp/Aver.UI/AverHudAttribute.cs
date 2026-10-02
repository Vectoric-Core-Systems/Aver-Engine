// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The marker that makes a class discoverable as a HUD.
namespace Aver.UI;

/// <summary>Marks a class the engine discovers as a HUD. It must have a public parameterless
/// constructor and a <c>public void Draw(float dt)</c>, both found by signature.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverHudAttribute : Attribute
{
    /// <summary>Marks a class as a HUD under the given display name.</summary>
    public AverHudAttribute(string name) => Name = name;

    /// <summary>The display name.</summary>
    public string Name { get; }

    /// <summary>Draw this one when the editor has no better reason to choose. Ties break on
    /// declaration order.</summary>
    public bool Default { get; init; }
}
