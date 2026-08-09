// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The P/Invoke declarations for the Aver.Scene C ABI, plus asset-identity hashing.

using System.Runtime.InteropServices;

namespace Aver.Scene;

/// <summary>The P/Invoke surface for the Aver.Scene C ABI. Strings cross as UTF-8 in both
/// directions; setters return 1 on success and 0 on a rejected request.</summary>
internal static class Native
{
    private const string Lib = "Aver.Scene";

    /// <summary>Resolves a dense field id from a qualified name. 0 == unknown.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_field([MarshalAs(UnmanagedType.LPUTF8Str)] string qualifiedName);
    /// <summary>The field's kind.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_field_kind(int f);
    /// <summary>The field's arity in floats.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_field_arity(int f);

    /// <summary>Reads an F32 field.</summary>
    [DllImport(Lib)] internal static extern float aver_scene_get_f32(int e, int f);
    /// <summary>Writes an F32 field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_f32(int e, int f, float v);
    /// <summary>Reads a float-kind field into <paramref name="outv"/>.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_get_vec(int e, int f, float[] outv);
    /// <summary>Writes a float-kind field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_vec(int e, int f, float[] v);
    /// <summary>Reads an I32 or Bool field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_get_i32(int e, int f);          // also BOOL
    /// <summary>Writes an I32 or Bool field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_i32(int e, int f, int v);
    /// <summary>Reads an I64 field.</summary>
    [DllImport(Lib)] internal static extern long aver_scene_get_i64(int e, int f);
    /// <summary>Writes an I64 field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_i64(int e, int f, long v);   // the ObjectId/mesh family
    /// <summary>Reads an entity-reference field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_get_ref(int e, int f);
    /// <summary>Writes an entity-reference field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_ref(int e, int f, int v);
    /// <summary>Reads a String field as a UTF-8 pointer; decode it through <see cref="Str"/>.</summary>
    [DllImport(Lib)] internal static extern System.IntPtr aver_scene_get_str(int e, int f);
    /// <summary>Writes a String field.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_str(int e, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);

    /// <summary>Creates an entity.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_create();
    /// <summary>Destroys an entity and its subtree.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_destroy(int e);
    /// <summary>Attaches a component to an entity.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_add_component(int e, int component);
    /// <summary>Reparents an entity.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_parent(int child, int parent);

    /// <summary>The entity's persisted identity, an fnv1a64 held in CName.objectId, not a field.</summary>
    [DllImport(Lib)] internal static extern long aver_scene_object_id(int e);
    /// <summary>Sets the entity's persisted identity.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_object_id(int e, long objectId);
    /// <summary>The entity's name as a UTF-8 pointer.</summary>
    [DllImport(Lib)] internal static extern System.IntPtr aver_scene_name(int e);
    /// <summary>Sets the entity's name.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_set_name(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>Resolves a material name to the opaque I32 handle CMeshRenderer.material stores.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_material(int name0, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>Decodes a UTF-8 string returned as a pointer; "?" if null.</summary>
    internal static string Str(System.IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "?";
}

/// <summary>Asset identity: the same fnv1a64 the engine's Assets layer stamps on an asset.</summary>
public static class Assets
{
    private const ulong FnvOffset = 0xcbf29ce484222325UL;
    private const ulong FnvPrime = 0x100000001b3UL;

    /// <summary>fnv1a64 of the UTF-8 bytes of <paramref name="path"/>. Empty path -> 0 (== unset).</summary>
    public static long ObjectIdOf(string path)
    {
        if (string.IsNullOrEmpty(path)) return 0;
        ulong h = FnvOffset;
        foreach (byte b in System.Text.Encoding.UTF8.GetBytes(path))
        {
            h ^= b;
            h *= FnvPrime;
        }
        return unchecked((long)h);
    }
}
