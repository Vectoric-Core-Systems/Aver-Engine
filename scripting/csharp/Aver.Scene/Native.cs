using System.Runtime.InteropServices;

namespace Aver.Scene;

/// <summary>
/// The P/Invoke surface for the Aver.Scene C ABI. Same idiom as <c>Pbr</c>/<c>Voxi</c>: a private
/// <see cref="Lib"/> const, exact export names, no <c>EntryPoint</c>/<c>CharSet</c>/<c>CallingConvention</c>
/// (defaults to Winapi), blittable types only, strings in as <see cref="UnmanagedType.LPUTF8Str"/> and out
/// as <see cref="System.IntPtr"/> decoded through <see cref="Str"/>.
/// </summary>
/// <remarks>
/// <para>
/// <b>Strings cross as UTF-8.</b> The world's name blob stores the raw <c>const char*</c> bytes it is
/// handed (see <c>World::setName</c>), and the engine's identity hash is fnv1a64 of those UTF-8 bytes.
/// So inbound strings marshal as <see cref="UnmanagedType.LPUTF8Str"/> and outbound pointers decode
/// through <see cref="Marshal.PtrToStringUTF8"/> — <see cref="UnmanagedType.LPStr"/> would re-encode a
/// non-ASCII name in the process ANSI code page, corrupting the bytes and the objectId derived from them.
/// </para>
/// <para>
/// <b>The 1/0 setter convention.</b> Every setter returns <c>int32_t</c>: 1 on success, 0 on a rejected
/// request (stale handle, wrong field kind, out of range). The managed facade reads that as
/// <c>!= 0</c>. Getters return the value, or a documented neutral value for a stale handle.
/// </para>
/// <para>
/// <b>Fields are addressed by a dense id, never by kind guesswork.</b> An <c>aver_field</c> resolved
/// from a qualified name ("CLocal.position") carries the component, the byte offset AND the kind, so a
/// set through the wrong-kind accessor is rejected by the ABI rather than silently mis-writing. This is
/// why there is one accessor family per kind and no generic "set bytes".
/// </para>
/// </remarks>
internal static class Native
{
    private const string Lib = "Aver.Scene";

    // Field resolution — a dense id (component + offset + kind), 0 == unknown.
    [DllImport(Lib)] internal static extern int aver_scene_field([MarshalAs(UnmanagedType.LPUTF8Str)] string qualifiedName);
    [DllImport(Lib)] internal static extern int aver_scene_field_kind(int f);
    [DllImport(Lib)] internal static extern int aver_scene_field_arity(int f);

    // Set/get by kind. Note there is BOTH a set_i32 and a first-class set_ref: an entity reference is
    // its own kind (Entity) with its own accessor, not an int32 in disguise. (See contradiction #2/#4.)
    [DllImport(Lib)] internal static extern float aver_scene_get_f32(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_f32(int e, int f, float v);
    [DllImport(Lib)] internal static extern int aver_scene_get_vec(int e, int f, float[] outv);
    [DllImport(Lib)] internal static extern int aver_scene_set_vec(int e, int f, float[] v);
    [DllImport(Lib)] internal static extern int aver_scene_get_i32(int e, int f);          // also BOOL
    [DllImport(Lib)] internal static extern int aver_scene_set_i32(int e, int f, int v);
    [DllImport(Lib)] internal static extern long aver_scene_get_i64(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_i64(int e, int f, long v);   // the ObjectId/mesh family
    [DllImport(Lib)] internal static extern int aver_scene_get_ref(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_ref(int e, int f, int v);
    [DllImport(Lib)] internal static extern System.IntPtr aver_scene_get_str(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_str(int e, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);

    // Entity lifetime + hierarchy. A model placed inside an actor is a plain child entity: it carries a
    // mesh but NO class, so aver_fw_class_of(child) == 0 — it is data, not an actor.
    [DllImport(Lib)] internal static extern int aver_scene_create();
    [DllImport(Lib)] internal static extern int aver_scene_destroy(int e);
    [DllImport(Lib)] internal static extern int aver_scene_add_component(int e, int component);
    [DllImport(Lib)] internal static extern int aver_scene_set_parent(int child, int parent);

    // Persisted identity lives in CName.objectId (a u64 fnv1a64), reached via these — NOT a component
    // field. This is the built shape (World::objectId/setObjectId); it is what a dragged gizmo keys on.
    [DllImport(Lib)] internal static extern long aver_scene_object_id(int e);
    [DllImport(Lib)] internal static extern int aver_scene_set_object_id(int e, long objectId);
    [DllImport(Lib)] internal static extern System.IntPtr aver_scene_name(int e);
    [DllImport(Lib)] internal static extern int aver_scene_set_name(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    // Content resolution at bind time. CMeshRenderer.material is an i32 opaque handle (built), so a
    // material NAME resolves to that handle here — it is never stored as a string in the field.
    [DllImport(Lib)] internal static extern int aver_scene_material(int name0, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>Decode a UTF-8 string returned as a pointer; "?" if null. The ABI hands out raw name-blob
    /// bytes, which are UTF-8, so this must decode as UTF-8 — <see cref="Marshal.PtrToStringAnsi"/> would
    /// mangle any non-ASCII name.</summary>
    internal static string Str(System.IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "?";
}

/// <summary>
/// Asset identity. An <see cref="ObjectIdOf"/> is the same fnv1a64 the engine's Assets layer uses, so a
/// mesh path hashed here equals the <c>u64</c> the content pipeline stamped on that asset.
/// </summary>
/// <remarks>
/// This is why <c>Aver.Framework.ActorBuilder.Place</c> can take a readable path in the generated
/// line yet store <c>CMeshRenderer.mesh</c> as an I64 ObjectId: the path is hashed in managed code, no
/// native round-trip, and the value written crosses the ABI through <c>set_i64</c> — the correct kind.
/// </remarks>
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
        return unchecked((long)h);   // the field is I64; identity, not magnitude, so the reinterpret is fine
    }
}
