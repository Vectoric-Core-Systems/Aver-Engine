namespace Aver.Scene;

/// <summary>
/// The dense ids the C# surface addresses the scene by. Component ids are the eight fixed built-ins
/// (Components.hpp, ids 1..8); field ids are resolved once by qualified name and cached, because
/// <c>aver_scene_field</c> is a lookup and the answer never changes for the life of the process.
/// </summary>
/// <remarks>
/// The qualified names below are asserted against the BUILT registration in <c>Builtins.cpp</c>, not
/// against the raw C++ struct members. That distinction matters in two places the design doc got wrong:
/// the light's tint is registered as <c>colour</c> (a British identifier the built surface already
/// uses), and <c>CMeshRenderer.mesh</c> is I64 while <c>material</c> is I32 — so the placement API sets
/// them with <c>set_i64</c>/<c>set_i32</c>, never <c>set_str</c> (contradiction #2).
/// </remarks>
public static class SceneIds
{
    // Fixed component ids — constants only because the built-ins register first (Components.hpp).
    public const int CLocal = 1;
    public const int CWorld = 2;
    public const int CHierarchy = 3;
    public const int CName = 4;
    public const int CTags = 5;
    public const int CMeshRenderer = 6;
    public const int CLight = 7;
    public const int CCamera = 8;

    private static readonly System.Collections.Generic.Dictionary<string, int> s_cache = new();

    /// <summary>Resolve (and cache) a dense field id by qualified name, e.g. "CLocal.position". 0 == unknown.</summary>
    public static int Field(string qualifiedName)
    {
        if (s_cache.TryGetValue(qualifiedName, out int id)) return id;
        id = Native.aver_scene_field(qualifiedName);
        s_cache[qualifiedName] = id;   // cache even a 0: an unknown name stays unknown this session
        return id;
    }

    // Transform (CLocal is flattened at registration into position/rotation/scale — there is no `xf`).
    public static int LocalPosition => Field("CLocal.position");   // Vec3, centimetres
    public static int LocalRotation => Field("CLocal.rotation");   // Quat
    public static int LocalScale => Field("CLocal.scale");         // Vec3

    // Mesh renderer.
    public static int MeshMesh => Field("CMeshRenderer.mesh");         // I64 asset ObjectId
    public static int MeshMaterial => Field("CMeshRenderer.material"); // I32 opaque handle
}
