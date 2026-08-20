// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The component and field ids the C# surface addresses the scene by.

namespace Aver.Scene;

/// <summary>The dense ids the C# surface addresses the scene by: the eight fixed built-in component
/// ids, and field ids resolved once by qualified name and cached. Names match Builtins.cpp.</summary>
public static class SceneIds
{
    /// <summary>Fixed component ids — constants because the built-ins register first (Components.hpp).</summary>
    /// <remarks>ALL ELEVEN, not the eight this stopped at. The missing three were the skinned/animated/
    /// particle trio, and nothing in C# could attach one: a component tree authored in a graph could ask
    /// for a SkeletalMesh and there was no id to add. Add a constant here whenever Components.hpp gains
    /// one — the numbers are fixed by that file, so a stale list here is silently short, never wrong.</remarks>
    public const int CLocal = 1;
    public const int CWorld = 2;
    public const int CHierarchy = 3;
    public const int CName = 4;
    public const int CTags = 5;
    public const int CMeshRenderer = 6;
    public const int CLight = 7;
    public const int CCamera = 8;
    public const int CSkeletalMesh = 9;
    public const int CAnimator = 10;
    public const int CParticleEmitter = 11;
    public const int CAttachment = 12;

    private static readonly System.Collections.Generic.Dictionary<string, int> s_cache = new();

    /// <summary>Resolves and caches a dense field id by qualified name, e.g. "CLocal.position". 0 == unknown.</summary>
    public static int Field(string qualifiedName)
    {
        if (s_cache.TryGetValue(qualifiedName, out int id)) return id;
        id = Native.aver_scene_field(qualifiedName);
        s_cache[qualifiedName] = id;   // cache even a 0: an unknown name stays unknown this session
        return id;
    }

    public static int LocalPosition => Field("CLocal.position");   // Vec3, centimetres
    public static int LocalRotation => Field("CLocal.rotation");   // Quat
    public static int LocalScale => Field("CLocal.scale");         // Vec3

    public static int MeshMesh => Field("CMeshRenderer.mesh");         // I64 asset ObjectId
    public static int MeshMaterial => Field("CMeshRenderer.material"); // I32 opaque handle
}
