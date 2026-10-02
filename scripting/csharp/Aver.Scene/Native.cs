// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
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

    /// <summary>Why the last scene call on THIS thread failed. See <see cref="Aver.Scene.SceneError"/>.</summary>
    [DllImport(Lib)] internal static extern int aver_scene_last_error();

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

/// <summary>Why a scene call failed, mirroring aver::AbiError in
/// modules/core/include/aver/core/ErrorCodes.hpp. Every value except <see cref="Ok"/> is negative,
/// so <c>(int)code &lt; 0</c> means "failed" even for a code added after this assembly was built.
/// </summary>
public enum SceneError
{
    /// <summary>No error was recorded by the last call that records one.</summary>
    Ok = 0,
    /// <summary>No field has that id, or the entity is dead or lacks the field's component.</summary>
    BadHandle = -1,
    /// <summary>A required out-parameter was null.</summary>
    NullPointer = -2,
    /// <summary>The module's world does not exist yet.</summary>
    NotInitialised = -3,
    /// <summary>An index past the end of what exists.</summary>
    OutOfRange = -4,
    /// <summary>The field is real and of the right kind, and is READ-ONLY. CWorld.matrix is the one
    /// people meet.</summary>
    Unsupported = -5,
    /// <summary>The field is real but of another kind than this accessor reads, or a name resolved to
    /// nothing.</summary>
    InvalidArgument = -6,
    /// <summary>The request was legal and the memory was not there.</summary>
    AllocationFailed = -7,
}

/// <summary>Reads the reason the last scene call failed.</summary>
///
/// <remarks>WHY THIS IS NOT ON THE CALL ITSELF. Every setter in the scene ABI returns 1 for success
/// and 0 for failure, and every caller here writes <c>if (Native.aver_scene_set_f32(...))</c>. A
/// negative code returned from those would be TRUE, silently inverting each of those call sites with
/// no compile error -- so the reason travels on its own entry point and nothing else changes.
///
/// THREAD-LOCAL. It is about the calling thread's own last failure, which is what makes it safe to
/// read while other threads are also calling in.</remarks>
public static class Scene
{
    /// <summary>Why the last scene call on this thread failed, or <see cref="SceneError.Ok"/>.</summary>
    public static SceneError LastError => (SceneError)Native.aver_scene_last_error();

    /// <summary>True when the last scene call on this thread recorded a failure. Written as
    /// <c>&lt; 0</c> rather than a switch, so a code added after this build still reads as a
    /// failure rather than falling through to "fine".</summary>
    public static bool LastCallFailed => Native.aver_scene_last_error() < 0;
}
