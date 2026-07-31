// The component and field ids the C# surface addresses the scene by.

namespace Aver.Scene;

/// <summary>The dense ids the C# surface addresses the scene by: the eight fixed built-in component
/// ids, and field ids resolved once by qualified name and cached. Names match Builtins.cpp.</summary>
public static class SceneIds
{
    // Fixed component ids — constants because the built-ins register first (Components.hpp).
    public const int CLocal = 1;
    public const int CWorld = 2;
    public const int CHierarchy = 3;
    public const int CName = 4;
    public const int CTags = 5;
    public const int CMeshRenderer = 6;
    public const int CLight = 7;
    public const int CCamera = 8;

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
