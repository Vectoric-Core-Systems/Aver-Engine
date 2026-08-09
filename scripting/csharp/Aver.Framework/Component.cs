// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The built-in component kinds a script can query or attach.

namespace Aver.Framework;

/// <summary>The built-in component kinds. Values match the scene's fixed AVER_SCENE_COMP_* ids.</summary>
public enum Component
{
    /// <summary>Transform (position/rotation/scale). Every entity has one at birth.</summary>
    Local = 1,
    /// <summary>The composed world matrix.</summary>
    World = 2,
    /// <summary>Parent/child links. Every entity has one at birth.</summary>
    Hierarchy = 3,
    /// <summary>Display name and persisted ObjectId. Every entity has one at birth.</summary>
    Name = 4,
    /// <summary>A 32-bit tag mask the gameplay layer gives meaning to.</summary>
    Tags = 5,
    /// <summary>A drawable mesh and material.</summary>
    MeshRenderer = 6,
    /// <summary>A light source.</summary>
    Light = 7,
    /// <summary>A camera.</summary>
    Camera = 8,
    /// <summary>Binds a skeleton to whatever this entity draws, so its mesh can be posed.</summary>
    SkeletalMesh = 9,
    /// <summary>A clip and the clock running it.</summary>
    Animator = 10,
}
