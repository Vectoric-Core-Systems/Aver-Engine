// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_timers_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void TimerDispatchFn(int handle, long token, IntPtr user);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate int EventDispatchFn(int subscription, long token, IntPtr user);

/// <summary>The timers and events C ABI (framework_timers_abi.h). Declared here, not in a bridge
/// assembly, for the same reason as <see cref="Fw"/>: only this assembly has the DllImport resolver.</summary>
internal static class FwTE
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_timers_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_set_timer_dispatch(TimerDispatchFn? fn, IntPtr user);
    [DllImport(Lib)] internal static extern int aver_fw_timer_set(float delay, float firstDelay, int maxFires, int flags, int owner, long token);
    [DllImport(Lib)] internal static extern int aver_fw_timer_cancel(int handle);
    [DllImport(Lib)] internal static extern int aver_fw_timer_cancel_owner(int owner);
    [DllImport(Lib)] internal static extern int aver_fw_timer_active(int handle);
    [DllImport(Lib)] internal static extern float aver_fw_timer_remaining(int handle);
    [DllImport(Lib)] internal static extern int aver_fw_timer_fire_count(int handle);
    [DllImport(Lib)] internal static extern int aver_fw_timer_set_paused(int handle, int paused);
    [DllImport(Lib)] internal static extern int aver_fw_timer_is_paused(int handle);
    [DllImport(Lib)] internal static extern int aver_fw_timer_reset(int handle, float delay);

    [DllImport(Lib)] internal static extern void aver_fw_timers_update(float rawDt);
    [DllImport(Lib)] internal static extern void aver_fw_timers_set_paused(int paused);
    [DllImport(Lib)] internal static extern int aver_fw_timers_paused();
    [DllImport(Lib)] internal static extern void aver_fw_timers_set_dilation(float scale);
    [DllImport(Lib)] internal static extern float aver_fw_timers_dilation();
    [DllImport(Lib)] internal static extern float aver_fw_timers_scaled_dt(float rawDt);
    [DllImport(Lib)] internal static extern long aver_fw_timers_time_us();
    [DllImport(Lib)] internal static extern long aver_fw_timers_real_time_us();
    [DllImport(Lib)] internal static extern int aver_fw_timers_count();
    [DllImport(Lib)] internal static extern int aver_fw_timers_epoch();
    [DllImport(Lib)] internal static extern void aver_fw_timers_clear();

    [DllImport(Lib)] internal static extern int aver_fw_set_event_dispatch(EventDispatchFn? fn, IntPtr user);
    [DllImport(Lib)] internal static extern int aver_fw_event_subscribe([MarshalAs(UnmanagedType.LPUTF8Str)] string name, int owner, int priority, int senderFilter, long token);
    [DllImport(Lib)] internal static extern int aver_fw_event_unsubscribe(int subscription);
    [DllImport(Lib)] internal static extern int aver_fw_event_unsubscribe_owner(int owner);
    [DllImport(Lib)] internal static extern int aver_fw_event_listener_count([MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    [DllImport(Lib)] internal static extern void aver_fw_event_payload_clear();
    [DllImport(Lib)] internal static extern int aver_fw_event_push_int(long v);
    [DllImport(Lib)] internal static extern int aver_fw_event_push_float(float v);
    [DllImport(Lib)] internal static extern int aver_fw_event_push_bool(int v);
    [DllImport(Lib)] internal static extern int aver_fw_event_push_string([MarshalAs(UnmanagedType.LPUTF8Str)] string v);
    [DllImport(Lib)] internal static extern int aver_fw_event_push_vec3(float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_fw_event_push_entity(int e);

    [DllImport(Lib)] internal static extern int aver_fw_event_dispatch([MarshalAs(UnmanagedType.LPUTF8Str)] string name, int sender, int target);
    [DllImport(Lib)] internal static extern int aver_fw_event_post([MarshalAs(UnmanagedType.LPUTF8Str)] string name, int sender, int target);

    [DllImport(Lib)] internal static extern int aver_fw_events_flush();
    [DllImport(Lib)] internal static extern int aver_fw_events_pending();
    [DllImport(Lib)] internal static extern int aver_fw_events_overflowed();
    [DllImport(Lib)] internal static extern void aver_fw_events_clear_pending();
    [DllImport(Lib)] internal static extern void aver_fw_events_clear();

    [DllImport(Lib)] internal static extern int aver_fw_event_current_valid();
    [DllImport(Lib)] internal static extern IntPtr aver_fw_event_current_name();
    [DllImport(Lib)] internal static extern int aver_fw_event_current_sender();
    [DllImport(Lib)] internal static extern int aver_fw_event_current_target();
    [DllImport(Lib)] internal static extern int aver_fw_event_current_arg_count();
    [DllImport(Lib)] internal static extern int aver_fw_event_current_arg_type(int index);
    [DllImport(Lib)] internal static extern long aver_fw_event_current_arg_int(int index);
    [DllImport(Lib)] internal static extern float aver_fw_event_current_arg_float(int index);
    [DllImport(Lib)] internal static extern int aver_fw_event_current_arg_bool(int index);
    [DllImport(Lib)] internal static extern int aver_fw_event_current_arg_entity(int index);
    [DllImport(Lib)] internal static extern IntPtr aver_fw_event_current_arg_string(int index);
    [DllImport(Lib)] internal static extern int aver_fw_event_current_arg_vec3(int index, float[] outXyz);
}
