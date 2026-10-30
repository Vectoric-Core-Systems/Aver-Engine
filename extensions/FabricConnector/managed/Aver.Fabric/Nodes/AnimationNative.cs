// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke into Aver.Anim.Abi (modules/anim.abi, propanim_abi.h): the property-track clips and the player AnimationPlayer drives. The enums restate the
// header's constants and are checked against it by AbiEnumTest; the declarations are checked against the header's parameter names.

using System.Runtime.InteropServices;

namespace Aver.Fabric;

/// <summary>The animation library's track types, as propanim_abi.h numbers them (AVER_PANIM_TRACK_*).</summary>
internal enum PanimTrackType { Value = 0, Position3D = 1, Rotation3D = 2, Scale3D = 3, BlendShape = 4, Method = 5, Bezier = 6, Audio = 7, Animation = 8 }

/// <summary>The value kinds the animation library stores (AVER_PANIM_KIND_*, aver::fmt::OcPropKind).</summary>
internal enum PanimKind { Nil = 0, Bool = 1, Int = 2, Float = 3, Vec2 = 4, Vec3 = 5, Vec4 = 6, Quat = 7, Color = 8, String = 9, Text = 10 }

/// <summary>AVER_PANIM_INTERP_*.</summary>
internal enum PanimInterp { Nearest = 0, Linear = 1, Cubic = 2, LinearAngle = 3, CubicAngle = 4 }

/// <summary>AVER_PANIM_UPDATE_*.</summary>
internal enum PanimUpdate { Continuous = 0, Discrete = 1, Capture = 2 }

/// <summary>AVER_PANIM_LOOP_*.</summary>
internal enum PanimLoop { None = 0, Linear = 1, Pingpong = 2 }

/// <summary>AVER_PANIM_DISCRETE_*.</summary>
internal enum PanimDiscrete { Dominant = 0, Recessive = 1, Force = 2 }

/// <summary>AVER_PANIM_EVENT_*.</summary>
internal enum PanimEvent { Started = 0, Finished = 1, Changed = 2, CurrentChanged = 3, Looped = 4, Method = 5, AudioPlay = 6, AudioStop = 7, AnimPlay = 8, AnimStop = 9, Applied = 10, MixerApplied = 11 }

/// <summary>The exports of Aver.Anim.Abi. The library is looked for beside the executable (NativeResolver); a host without it makes
/// <see cref="Available"/> false and the animation classes quietly do nothing.</summary>
internal static class PanimNative
{
    private const string Lib = "Aver.Anim.Abi";

    private static int s_state; // 0 unknown, 1 loaded, 2 missing

    internal static bool Available
    {
        get
        {
            if (s_state == 0)
            {
                try { s_state = aver_panim_abi_version() >= 1 ? 1 : 2; }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { s_state = 2; }
            }
            return s_state == 1;
        }
    }

    internal static string Str(IntPtr p) => PplayerNative.Str(p);

    // The library is single-threaded (propanim_abi.h), and a finalizer runs on another thread: it only queues the handle, and the next call
    // from the game thread that makes or steps an animation object destroys it.
    private static readonly System.Collections.Concurrent.ConcurrentQueue<int> s_deadClips = new();
    private static readonly System.Collections.Concurrent.ConcurrentQueue<int> s_deadPlayers = new();

    internal static void QueueDestroyClip(int handle) => s_deadClips.Enqueue(handle);
    internal static void QueueDestroyPlayer(int handle) => s_deadPlayers.Enqueue(handle);

    /// <summary>Destroys the clips and players whose objects were collected. Call from the game thread.</summary>
    internal static void DrainDead()
    {
        if (s_state != 1) return;
        while (s_deadClips.TryDequeue(out int c)) aver_panim_destroy(c);
        while (s_deadPlayers.TryDequeue(out int p)) PplayerNative.aver_pplayer_destroy(p);
    }

    /// <summary>Forgets a failed probe (a test that has just copied the library beside itself).</summary>
    internal static void Reprobe() { if (s_state == 2) s_state = 0; }

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_abi_version();

    // ---- clip ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_create();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_panim_destroy(int clip);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_clone(int clip);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_load([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_save(int clip, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_set_length(int clip, double seconds);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_panim_length(int clip);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_set_loop_mode(int clip, int mode);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_loop_mode(int clip);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_set_step(int clip, double seconds);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_panim_step(int clip);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_count(int clip);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_add(int clip, int type, int atPosition);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_remove(int clip, int track);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_move(int clip, int track, int toPosition);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_type(int clip, int track);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_set_path(int clip, int track, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_panim_track_path(int clip, int track);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_set_interp(int clip, int track, int interp);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_interp(int clip, int track);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_set_update(int clip, int track, int update);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_update(int clip, int track);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_set_flags(int clip, int track, int loopWrap, int enabled, int imported);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_track_flags(int clip, int track);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_count(int clip, int track);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_insert(int clip, int track, double time, double[] v5, [MarshalAs(UnmanagedType.LPUTF8Str)] string text, double transition);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_remove(int clip, int track, int key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_panim_key_time(int clip, int track, int key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_set_time(int clip, int track, int key, double time);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_panim_key_transition(int clip, int track, int key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_set_transition(int clip, int track, int key, double transition);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_value(int clip, int track, int key, double[] v5);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_panim_key_text(int clip, int track, int key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_set_value(int clip, int track, int key, double[] v5, [MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_find(int clip, int track, double time, int mode, int backward, int limit);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_sample(int clip, int track, double time, int backward, double[] v5);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_panim_sample_text();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_set_handles(int clip, int track, int key, double inX, double inY, double outX, double outY);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_handles(int clip, int track, int key, double[] out4);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_arg_count(int clip, int track, int key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_arg_set(int clip, int track, int key, int index, double[] v5, [MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_arg_get(int clip, int track, int key, int index, double[] v5);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_panim_key_arg_text(int clip, int track, int key, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_args_clear(int clip, int track, int key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_set_offsets(int clip, int track, int key, double start, double end);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_panim_key_offsets(int clip, int track, int key, double[] out2);
}

/// <summary>The player half of Aver.Anim.Abi.</summary>
internal static class PplayerNative
{
    private const string Lib = "Aver.Anim.Abi";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_create();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_pplayer_destroy(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_clip(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, int clip);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_remove_clip(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_has_clip(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_clip_count(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_clip_key(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_blend_time(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string from, [MarshalAs(UnmanagedType.LPUTF8Str)] string to, double seconds);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_blend_time(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string from, [MarshalAs(UnmanagedType.LPUTF8Str)] string to);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_next(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string from, [MarshalAs(UnmanagedType.LPUTF8Str)] string to);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_next(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string from);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_speed_scale(int player, double scale);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_speed_scale(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_default_blend(int player, double seconds);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_default_blend(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_deterministic(int player, int on);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_deterministic(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_active(int player, int on);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_active(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_discrete_mode(int player, int mode);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_discrete_mode(int player);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_play(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, double customBlend, double customSpeed, int fromEnd);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_stop(int player, int keepState);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_pause(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_queue(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_clear_queue(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_queue_count(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_queue_at(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_seek(int player, double seconds, int update, int updateOnly);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_advance(int player, double delta);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_assigned(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_is_playing(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_has_current(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_assigned(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_current(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_position(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_length(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_playing_speed(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_custom_speed(int player);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_capture_track_count(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_capture_track_type(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_capture_track_path(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_capture_begin(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_capture_add(int player, int type, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, double[] v5, [MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_capture_end(int player, double duration);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_event_count(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_event_type(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_event_a(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_event_b(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_event_path(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_event_t0(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_pplayer_event_t1(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_event_arg_count(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_event_arg(int player, int index, int arg, double[] v5);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_event_arg_text(int player, int index, int arg);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_clear_events(int player);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_output_count(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_output_track(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_output_discrete(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_output_value(int player, int index, double[] v5);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_output_text(int player, int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_target_type(int player, int target);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_target_path(int player, int target);

    // ---- tree mode (AnimationTree) ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_tree_add(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, double prev, double delta, double weight, double[] trackWeights, int trackWeightCount, int seeked, int external, int includeStart, double[] outNext);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_tree_mix(int player, int updateOnly);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_set_root_motion(int player, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_pplayer_root_motion_path(int player);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_pplayer_root_motion(int player, double[] out20);

    internal static string Str(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
}
