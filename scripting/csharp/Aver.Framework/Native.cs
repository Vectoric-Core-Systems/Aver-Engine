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

    // NAMED ACTIONS (minor 5, framework_abi.h) -- the port of this assembly's own EnhancedInput.cs
    // onto the framework, so a graph node or a C++ system can finally reach it too. held/pressed/
    // released/value2 read InputState's cur/prev ON DEMAND, with no per-frame roll of their own --
    // see EnhancedInput.cs's own top-of-file comment for why that is a hard requirement, not a
    // convenience, and why EnhancedInput.Update() (HostBridge.cs's old DispTickAll call site) is gone.
    [DllImport(Lib)] internal static extern int  aver_fw_action_register([MarshalAs(UnmanagedType.LPUTF8Str)] string name, int valueType);
    [DllImport(Lib)] internal static extern int  aver_fw_action_find([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern void aver_fw_action_bind(int action, int source, int key, float scale, int component, int contextPriority);
    [DllImport(Lib)] internal static extern void aver_fw_action_clear_bindings();
    [DllImport(Lib)] internal static extern void aver_fw_action_value2(int action, float[] out2);
    [DllImport(Lib)] internal static extern int  aver_fw_action_held(int action);
    [DllImport(Lib)] internal static extern int  aver_fw_action_pressed(int action);
    [DllImport(Lib)] internal static extern int  aver_fw_action_released(int action);

    // RAW WIN32 VK -- an additive twin to aver_fw_input_key above, indexed by the literal Win32 VK
    // code (0..255) instead of the AVER_FW_KEY_* enum, for the F-keys/numpad/OEM range that enum can
    // never grow to cover (InputKeys.hpp's own comment: a saved .ocgraph's InputKey node stores the
    // enum's current int, so inserting a new key anywhere but the tail would repoint every saved
    // graph at the wrong key). set_vk has no caller in this assembly today, same as
    // aver_fw_input_set_key above -- it exists so a test can round-trip it by reflection exactly as
    // NewNodeTests.cs already does for aver_fw_input_set_key/aver_fw_input_key.
    [DllImport(Lib)] internal static extern void aver_fw_input_set_vk(int vk, int down);
    [DllImport(Lib)] internal static extern int  aver_fw_input_vk(int vk);
    [DllImport(Lib)] internal static extern int  aver_fw_input_vk_pressed(int vk);
    [DllImport(Lib)] internal static extern int  aver_fw_input_vk_released(int vk);

    // GAMEPAD, SHAPE ONLY (framework_abi.h's own GAMEPAD section) -- no poller exists anywhere yet
    // (ZERO CONSUMERS was the stated reason not to build one), so the getters below read back
    // whatever a future provider sets, or the all-zero/false default nothing has ever written. `pad`
    // is fixed at 0 by the ABI itself; Input.cs's GetGamepadButton/GetGamepadAxis still take it as a
    // parameter rather than hardcoding it, so this signature does not change the day a second pad
    // becomes real.
    [DllImport(Lib)] internal static extern void  aver_fw_input_set_gamepad_button(int pad, int button, int down);
    [DllImport(Lib)] internal static extern void  aver_fw_input_set_gamepad_axis(int pad, int axis, float value);
    [DllImport(Lib)] internal static extern int   aver_fw_input_gamepad_button(int pad, int button);
    [DllImport(Lib)] internal static extern float aver_fw_input_gamepad_axis(int pad, int axis);

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

/// <summary>The Aver.Settings C ABI -- durable key/value a shipped game can read and write
/// (settings_abi.h). A SEPARATE class from <see cref="Fw"/>, not a section bolted onto it: these are
/// aver_settings_* exports out of a different native binary (Aver.Settings.dll) that settings_abi.h's
/// own header says "depends on nothing above Core and Platform" -- filing them under Fw would misname
/// every one of these as a framework call when it links no framework at all. SceneNative just above is
/// the same move for the identical reason, one native surface per DLL, all living in this one file
/// because that is where the CRITICAL CONSTRAINT this repo enforces says a new native call belongs.
///
/// UNLIKE Fw and SceneNative, this class needs NO entry in NativeResolver.cs. That resolver exists
/// only because "Aver.Framework" and "Aver.Scene" each name BOTH a managed contract assembly and a
/// native DLL sitting in the same output directory, so the default P/Invoke probe would find the
/// managed one first and GetProcAddress every symbol into failure (NativeResolver.cs's own comment).
/// There is no managed "Aver.Settings" assembly anywhere in scripting/csharp for "Aver.Settings" to
/// collide with, and modules/settings/CMakeLists.txt:16-18 stages the native Aver.Settings.dll into
/// the identical CMAKE_BINARY_DIR/bin the framework DLL already resolves into (framework/CMakeLists.txt
/// :26's own "DLL next to the exe") -- so the CLR's ordinary default probing finds it unassisted.</summary>
internal static class SettingsNative
{
    private const string Lib = "Aver.Settings";

    // Reading a file that does not exist is not an error -- a first run has no settings. Calling
    // this again with a different path closes the first WITHOUT flushing it (settings_abi.h's own
    // aver_settings_open comment) -- Settings.cs's Open() documents that for the caller.
    [DllImport(Lib)] internal static extern int aver_settings_open([MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);
    // Returned pointer must not be freed by the caller and is valid only until the next call --
    // decoded immediately through Fw.Str, the same convention aver_fw_class_name's callers already
    // follow for an identical lifetime.
    [DllImport(Lib)] internal static extern IntPtr aver_settings_default_path();
    [DllImport(Lib)] internal static extern int aver_settings_flush();

    // Readers: each returns `fallback` for a missing key OR a value that fails to parse as the
    // requested type (settings_abi.h's own comment -- a hand-edited `volume=loud` reads as the
    // fallback, never a silent 0).
    [DllImport(Lib)] internal static extern float aver_settings_get_f32([MarshalAs(UnmanagedType.LPUTF8Str)] string key, float fallback);
    [DllImport(Lib)] internal static extern int aver_settings_get_i32([MarshalAs(UnmanagedType.LPUTF8Str)] string key, int fallback);
    [DllImport(Lib)] internal static extern int aver_settings_get_bool([MarshalAs(UnmanagedType.LPUTF8Str)] string key, int fallback);
    [DllImport(Lib)] internal static extern IntPtr aver_settings_get_str([MarshalAs(UnmanagedType.LPUTF8Str)] string key, [MarshalAs(UnmanagedType.LPUTF8Str)] string fallback);

    // Writers: 1 on success, 0 for an empty key or a value that cannot be stored. Dirty until the
    // next aver_settings_flush -- nothing here touches disk by itself.
    [DllImport(Lib)] internal static extern int aver_settings_set_f32([MarshalAs(UnmanagedType.LPUTF8Str)] string key, float value);
    [DllImport(Lib)] internal static extern int aver_settings_set_i32([MarshalAs(UnmanagedType.LPUTF8Str)] string key, int value);
    [DllImport(Lib)] internal static extern int aver_settings_set_bool([MarshalAs(UnmanagedType.LPUTF8Str)] string key, int value);
    // A value containing a newline is REFUSED, not escaped -- the file is one key=value line each,
    // with no escaping at all, and a smuggled newline would silently become a second key.
    [DllImport(Lib)] internal static extern int aver_settings_set_str([MarshalAs(UnmanagedType.LPUTF8Str)] string key, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    [DllImport(Lib)] internal static extern int aver_settings_has([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    // 1 if the key is gone afterwards, INCLUDING when it never existed -- Remove() is not a "did I
    // do anything" flag, it is a postcondition check.
    [DllImport(Lib)] internal static extern int aver_settings_remove([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_settings_count();
}
