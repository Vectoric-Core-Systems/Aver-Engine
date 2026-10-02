// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// The base class a gameplay script derives from.
namespace Aver.Scripting;

/// <summary>Base class for a gameplay script. The scripting host finds derived types by reflection.</summary>
public abstract class AverBehaviour
{
    /// <summary>Called once, immediately after the behaviour is constructed.</summary>
    public virtual void OnStart() { }

    /// <summary>Called once per frame. <paramref name="dt"/> is seconds since the last frame.</summary>
    public virtual void OnUpdate(float dt) { }

    /// <summary>Called once as the host shuts down, or when the assembly is about to be unloaded.</summary>
    public virtual void OnShutdown() { }
}
