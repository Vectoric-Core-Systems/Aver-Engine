// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// P/Invoke declarations for the framework C ABI and the slice of the scene C ABI the framework calls.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The framework C ABI (the <c>aver_fw_*</c> exports of framework_abi.h).</summary>
/// <remarks>Strings are UTF-8 both ways; every handle is an int32 with 0 == invalid.</remarks>
internal static class Fw
{
    private const string Lib = "Aver.Framework";

    // class_declare is idempotent by name: the same string returns the same handle for the life of the process.
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

    // Class defaults, addressed by the same dense field id the scene resolves. One setter per storable kind.
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_f32(int c, int f, float v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_i32(int c, int f, int v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_i64(int c, int f, long v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_vec(int c, int f, float[] v);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_str(int c, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);

    // GameMode wiring, resolved by class name at seal.
    [DllImport(Lib)] internal static extern int aver_fw_class_set_default_pawn(int gameMode, [MarshalAs(UnmanagedType.LPUTF8Str)] string pawnClassName);
    [DllImport(Lib)] internal static extern int aver_fw_class_set_player_controller(int gameMode, [MarshalAs(UnmanagedType.LPUTF8Str)] string controllerClassName);

    // Spawn: rotation crosses as a quaternion; a null pos/quat/scale means "use the class default".
    [DllImport(Lib)] internal static extern int aver_fw_spawn(int c, [MarshalAs(UnmanagedType.LPUTF8Str)] string? name, float[]? pos3, float[]? quat4, float[]? scale3);
    [DllImport(Lib)] internal static extern int aver_fw_destroy(int e);
    // ANIMATION CURVES, relayed. The framework does not know what an animation is; a provider
    // installed by the composition root answers this. See framework_abi.h for why it is a relay.
    [DllImport(Lib)] internal static extern int aver_fw_anim_curve(int entity, long nameHash, out float outValue);
    // SYNAPSE STEERING TARGET, relayed -- same shape as aver_fw_anim_curve immediately above, for
    // the identical reason (framework_abi.h).
    [DllImport(Lib)] internal static extern int aver_fw_synapse_target(int entity, out float outX, out float outY, out float outZ);
    // SYNAPSE PERCEPTION, relayed -- same reason, but 0/false does NOT mean "nothing to report"
    // here; see framework_abi.h's own comment on aver_fw_synapse_perception for why the RETURN
    // value and outCanSee carry two different questions.
    [DllImport(Lib)] internal static extern int aver_fw_synapse_perception(int entity, out int outCanSee, out int outLastTarget, out float outTimeSinceSeen);
    // SAVE/LOAD, relayed. The framework does not know what a save file is; a provider installed by
    // the composition root answers these. See framework_abi.h for why it is a relay.
    [DllImport(Lib)] internal static extern int aver_fw_save_write([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib)] internal static extern int aver_fw_save_load([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    // GRAPH-LOCAL VARIABLES, relayed -- same shape as anim_curve/save_write just above. Declared HERE,
    // not in Aver.Scripting.Bridge, for a reason that cost a real EntryPointNotFoundException to find:
    // ONLY this assembly has a DllImportResolver registered (NativeResolver.cs) routing "Aver.Framework"
    // to the native DLL one directory up. A raw [DllImport("Aver.Framework")] declared directly in the
    // Bridge assembly instead resolves through default probing, which finds bin/Scripting/Aver.Framework.dll
    // first -- the MANAGED assembly of the same name sitting right next to the bridge -- and GetProcAddress
    // against a pure-IL PE fails for every symbol. Every native call from HostBridge.cs must go through
    // Fw.* (this class) for exactly this reason; see ManagedDispatch.Install for the identical precedent.
    [DllImport(Lib)] internal static extern int aver_fw_set_graph_var_provider(IntPtr count, IntPtr at, IntPtr setVar, IntPtr user);
    [DllImport(Lib)] internal static extern int aver_fw_class_of(int e);   // != 0 IS the definition of "actor"

    // Possession is rejected unless the two classes carry the PAWN / CONTROLLER flags.
    [DllImport(Lib)] internal static extern int aver_fw_possess(int controller, int pawn);
    [DllImport(Lib)] internal static extern int aver_fw_unpossess(int controller);
    [DllImport(Lib)] internal static extern int aver_fw_controlled_pawn(int controller);
    [DllImport(Lib)] internal static extern int aver_fw_controller_of(int pawn);

    // Session singletons.
    [DllImport(Lib)] internal static extern int aver_fw_game_instance();
    [DllImport(Lib)] internal static extern int aver_fw_game_mode();
    [DllImport(Lib)] internal static extern int aver_fw_player_controller(int playerIndex);
    [DllImport(Lib)] internal static extern int aver_fw_play_state();

    // Input: the app pushes new_frame/set_key/set_mouse; gameplay reads key/pressed/released/mouse.
    [DllImport(Lib)] internal static extern void aver_fw_input_new_frame();
    [DllImport(Lib)] internal static extern void aver_fw_input_set_key(int key, int down);
    [DllImport(Lib)] internal static extern void aver_fw_input_set_mouse(float dx, float dy, float wheel);
    [DllImport(Lib)] internal static extern int aver_fw_input_key(int key);
    [DllImport(Lib)] internal static extern int aver_fw_input_key_pressed(int key);
    [DllImport(Lib)] internal static extern int aver_fw_input_key_released(int key);
    [DllImport(Lib)] internal static extern void aver_fw_input_mouse(float[] out3);
    [DllImport(Lib)] internal static extern void aver_fw_set_view(int mode, float eyeHeight, float boomLength);
    [DllImport(Lib)] internal static extern void aver_fw_set_view_entity(int entity);

    [DllImport(Lib)] internal static extern void aver_fw_set_sky_clouds(
        int seed, float coverage, float density, float bottomCm, float topCm,
        float featureScale, float windXCmPerSec, float windYCmPerSec);
    [DllImport(Lib)] internal static extern void aver_fw_clear_sky_clouds();
    [DllImport(Lib)] internal static extern int  aver_fw_view_entity();

    // FLUID VOLUME SPAWN, relayed -- see framework_abi.h's own aver_fw_fluid_spawn comment for why
    // this queues rather than spawning synchronously, and for why there is no subdivisions=
    // parameter. `name` is a label for the host's own log, not a lookup key. Used by both
    // Aver.Graph's GraphComponentTree (a `COMP ... Fluid` line) and Game.SpawnFluidVolume (a
    // plain C# script) -- see Game.cs for the one call site both route through.
    [DllImport(Lib)] internal static extern int aver_fw_fluid_spawn(
        float cx, float cy, float cz, float hx, float hy, float hz,
        float compliance, float damping, int iterations, float pressure,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    // MATERIAL-LAYER SPAWN, relayed -- see framework_abi.h's own aver_fw_fluid_spawn_material
    // comment (MINOR 4) for the density/viscosity/preset sentinel and preset-name semantics. A
    // second, additive entry point beside aver_fw_fluid_spawn above, not a replacement for it.
    [DllImport(Lib)] internal static extern int aver_fw_fluid_spawn_material(
        float cx, float cy, float cz, float hx, float hy, float hz,
        float compliance, float damping, int iterations, float pressure,
        float densityKgM3, float viscosityPaS,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string materialPreset,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    /// <summary>Decodes a UTF-8 string returned as a pointer; "?" if null.</summary>
    internal static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "?";
}

/// <summary>The slice of the Aver.Scene C ABI the framework calls directly.</summary>
internal static class SceneNative
{
    private const string Lib = "Aver.Scene";

    [DllImport(Lib)] internal static extern int aver_scene_create();
    [DllImport(Lib)] internal static extern int aver_scene_destroy(int e);
    [DllImport(Lib)] internal static extern int aver_scene_add_component(int e, int component);
    [DllImport(Lib)] internal static extern int aver_scene_has_component(int e, int component);
    [DllImport(Lib)] internal static extern int aver_scene_set_parent(int child, int parent);

    // Hierarchy queries and enumeration.
    [DllImport(Lib)] internal static extern int aver_scene_parent(int e);
    [DllImport(Lib)] internal static extern int aver_scene_first_child(int e);
    [DllImport(Lib)] internal static extern int aver_scene_next_sibling(int e);
    [DllImport(Lib)] internal static extern int aver_scene_child_count(int e);
    [DllImport(Lib)] internal static extern int aver_scene_count();
    [DllImport(Lib)] internal static extern int aver_scene_at(int index);

    // Typed field get/set over the whole component surface.
    [DllImport(Lib)] internal static extern int aver_scene_field_arity(int f);
    [DllImport(Lib)] internal static extern float aver_scene_get_f32(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_f32(int e, int f, float v);
    [DllImport(Lib)] internal static extern int aver_scene_get_i32(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_i32(int e, int f, int v);
    [DllImport(Lib)] internal static extern long aver_scene_get_i64(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_i64(int e, int f, long v);
    [DllImport(Lib)] internal static extern int aver_scene_get_vec(int e, int f, float[] outv);
    [DllImport(Lib)] internal static extern int aver_scene_set_vec(int e, int f, float[] v);
    [DllImport(Lib)] internal static extern IntPtr aver_scene_get_str(int e, int f);
    [DllImport(Lib)] internal static extern int aver_scene_set_str(int e, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);

    [DllImport(Lib)] internal static extern int aver_scene_valid(int e);
    [DllImport(Lib)] internal static extern long aver_scene_object_id(int e);
    [DllImport(Lib)] internal static extern int aver_scene_set_object_id(int e, long objectId);
    [DllImport(Lib)] internal static extern IntPtr aver_scene_name(int e);
    [DllImport(Lib)] internal static extern int aver_scene_set_name(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    // world_matrix writes a row-major 4x4 with the translation in row 3.
    [DllImport(Lib)] internal static extern int aver_scene_find([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int aver_scene_world_matrix(int e, float[] out16);

    // material: name0 is the content-pack id (0 == the default/project pack).
    [DllImport(Lib)] internal static extern int aver_scene_material(int name0, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
}
