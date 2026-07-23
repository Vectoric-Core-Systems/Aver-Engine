namespace Aver.Framework;

/// <summary>
/// The built-in component kinds, matching the scene's fixed ids (AVER_SCENE_COMP_*). Passed to
/// <see cref="Entity.HasComponent"/> / <see cref="Entity.AddComponent"/>. The values are stable — a
/// generated constant file asserts against them — so they are safe to persist or compare.
/// </summary>
public enum Component
{
    /// <summary>Transform (position/rotation/scale). Every entity has one at birth.</summary>
    Local = 1,
    /// <summary>The composed world matrix (derived; one pass writes it).</summary>
    World = 2,
    /// <summary>Parent/child links. Every entity has one at birth.</summary>
    Hierarchy = 3,
    /// <summary>Display name / persisted ObjectId. Every entity has one at birth.</summary>
    Name = 4,
    /// <summary>A 32-bit tag mask the gameplay layer gives meaning to.</summary>
    Tags = 5,
    /// <summary>A drawable mesh + material.</summary>
    MeshRenderer = 6,
    /// <summary>A light source.</summary>
    Light = 7,
    /// <summary>A camera.</summary>
    Camera = 8,
}
