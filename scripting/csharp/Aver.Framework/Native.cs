using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>
/// The framework C ABI (framework_abi.h, the <c>aver_fw_*</c> exports). Same idiom as Aver.Scripting's
/// Pbr/Voxi and Aver.Scene's Native: a private <see cref="Lib"/> const, exact export names, no
/// EntryPoint/CharSet/CallingConvention (defaults to Winapi), blittable types only, the 1/0 setter
/// convention read as <c>!= 0</c>.
/// </summary>
/// <remarks>
/// <para>
/// <b>Strings are UTF-8 both ways.</b> Class names, parent names and mesh paths marshal as
/// <see cref="UnmanagedType.LPUTF8Str"/> and decode through <see cref="Str"/> with
/// <c>PtrToStringUTF8</c>. This follows scene_abi.h/framework_abi.h, which mandate UTF-8 in both
/// directions, and the reference binding in SCENE_FRAMEWORK.md §6.2. (The in-progress
/// <c>Aver.Scene/Native.cs</c> currently uses ANSI <c>LPStr</c>; that is the defect, and it must be
/// flipped to UTF-8 to match — see DESIGNER_REWRITE.md, contradiction ledger.)
/// </para>
/// <para>
/// <b>Every handle is an <c>int</c>, 0 == invalid.</b> Class, entity, component and field all cross as
/// <c>int32_t</c>; a live entity is guaranteed positive (bit 31 is reserved clear in Entity.hpp), so a
/// default handle is invalid with no signedness argument.
/// </para>
/// </remarks>
internal static class Fw
{
    private const string Lib = "Aver.Framework";

    // --- Class registry. declare() is IDEMPOTENT BY NAME: the same string returns the same handle for
    //     the life of the process. That stable handle is the whole of hot-reload identity. ---
    [DllImport(Lib)] internal static extern int aver_fw_class_declare([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string parentName);
    [DllImport(Lib)] internal static extern int aver_fw_class_find([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern IntPtr aver_fw_class_name(int c);
    [DllImport(Lib)] internal static extern int aver_fw_class_parent(int c);
    [DllImport(Lib)] internal static extern int aver_fw_class_reset(int c);
    [DllImport(Lib)] internal static extern int aver_fw_class_add_component(int c, int component);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_flags(int c, int flags);
    [DllImport(Lib)] internal static extern int aver_fw_class_get_flags(int c);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_tick(int c, int tickGroup, int tickOrder);
    [DllImport(Lib)] internal static extern int aver_fw_class_seal(int c);

    // Class defaults, addressed by the SAME dense field id the scene resolves. FIVE setters, one per
    // storable kind. There is no set_default_bool (bool rides i32) and no set_default_ref (an entity
    // default is meaningless in an archetype — it is per-instance). The kind is validated by the ABI.
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_f32(int c, int f, float v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_i32(int c, int f, int v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_i64(int c, int f, long v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_vec(int c, int f, float[] v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_str(int c, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);

    // GameMode wiring, resolved by class NAME at seal so two game classes never take a compile-time
    // reference to one another.
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_pawn(int gameMode, [MarshalAs(UnmanagedType.LPUTF8Str)] string pawnClassName);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_player_controller(int gameMode, [MarshalAs(UnmanagedType.LPUTF8Str)] string controllerClassName);

    // --- Actors. rotation crosses as a QUATERNION (quat4) though the author writes degrees. A null
    //     pos/quat/scale means "use the class default". ---
    [DllImport(Lib)] internal static extern int aver_fw_spawn(int c, [MarshalAs(UnmanagedType.LPUTF8Str)] string? name, float[]? pos3, float[]? quat4, float[]? scale3);
    [DllImport(Lib)] internal static extern int aver_fw_destroy(int e);
    [DllImport(Lib)] internal static extern int aver_fw_class_of(int e);   // != 0 IS the definition of "actor"

    // --- Possession. Rejected unless the two classes carry the PAWN / CONTROLLER flags — that flag
    //     check is the whole of type safety here, which is why the base types set the flags for you. ---
    [DllImport(Lib)] internal static extern int aver_fw_possess(int controller, int pawn);
    [DllImport(Lib)] internal static extern int aver_fw_unpossess(int controller);
    [DllImport(Lib)] internal static extern int aver_fw_controlled_pawn(int controller);
    [DllImport(Lib)] internal static extern int aver_fw_controller_of(int pawn);

    // --- Session singletons. ---
    [DllImport(Lib)] internal static extern int aver_fw_game_instance();
    [DllImport(Lib)] internal static extern int aver_fw_game_mode();
    [DllImport(Lib)] internal static extern int aver_fw_player_controller(int playerIndex);
    [DllImport(Lib)] internal static extern int aver_fw_play_state();

    /// <summary>Decode a UTF-8 string returned as a pointer; "?" if null. Duplicated per the tree idiom.</summary>
    internal static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "?";
}

/// <summary>
/// The small slice of the Aver.Scene C ABI the framework calls directly — transform read/write, name
/// and persisted identity, and the child-entity plumbing a placed model needs. Declared here (against
/// the same <c>Aver.Scene</c> native library) rather than reaching into <c>Aver.Scene.Native</c>, whose
/// members are <c>internal</c>: duplicating a P/Invoke across assemblies is the established idiom, and it
/// keeps this assembly's boundary explicit.
/// </summary>
/// <remarks>
/// Field ids come from <see cref="Aver.Scene.SceneIds"/> (public), so a component/field index can never
/// drift from the built registration. Strings are UTF-8, matching the header (see the note on
/// <see cref="Fw"/>).
/// </remarks>
internal static class SceneNative
{
    private const string Lib = "Aver.Scene";

    [DllImport(Lib)] internal static extern int aver_scene_create();
    [DllImport(Lib)] internal static extern int aver_scene_destroy(int e);
    [DllImport(Lib)] internal static extern int aver_scene_add_component(int e, int component);
    [DllImport(Lib)] internal static extern int aver_scene_set_parent(int child, int parent);

    [DllImport(Lib)] internal static extern int aver_scene_get_vec(int e, int f, float[] outv);
    [DllImport(Lib)] internal static extern int aver_scene_set_vec(int e, int f, float[] v);
    [DllImport(Lib)] internal static extern int aver_scene_set_i32(int e, int f, int v);
    [DllImport(Lib)] internal static extern int aver_scene_set_i64(int e, int f, long v);

    [DllImport(Lib)] internal static extern long aver_scene_object_id(int e);
    [DllImport(Lib)] internal static extern int aver_scene_set_object_id(int e, long objectId);
    [DllImport(Lib)] internal static extern IntPtr aver_scene_name(int e);
    [DllImport(Lib)] internal static extern int aver_scene_set_name(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    // A material NAME resolves to CMeshRenderer.material's i32 opaque handle here — never stored as a
    // string in the field. name0 is the content-pack id (0 == the default/project pack).
    [DllImport(Lib)] internal static extern int aver_scene_material(int name0, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
}
