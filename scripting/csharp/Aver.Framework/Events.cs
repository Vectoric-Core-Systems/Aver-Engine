// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Managed face of the framework event bus. See docs/GAME_TIMERS_EVENTS.md.

using System.Runtime.InteropServices;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework;

/// <summary>Payload value kinds. Pinned to AVER_FW_EVENT_* in framework_timers_abi.h.</summary>
public enum EventValueKind
{
    /// <summary>No value.</summary>
    None = 0,
    /// <summary>64-bit integer.</summary>
    Int = 1,
    /// <summary>Single-precision float.</summary>
    Float = 2,
    /// <summary>Boolean.</summary>
    Bool = 3,
    /// <summary>UTF-8 string.</summary>
    String = 4,
    /// <summary>Three floats.</summary>
    Vec3 = 5,
    /// <summary>A scene entity handle.</summary>
    Entity = 6,
}

/// <summary>One payload argument. Numeric kinds (Int, Float, Bool, Entity) read as each other.</summary>
public readonly struct EventValue
{
    /// <summary>What was stored.</summary>
    public EventValueKind Kind { get; init; }
    private readonly long _i;
    private readonly float _x, _y, _z;
    private readonly string? _s;

    internal EventValue(EventValueKind kind, long i, float x, float y, float z, string? s)
    { Kind = kind; _i = i; _x = x; _y = y; _z = z; _s = s; }

    /// <summary>The value as an integer (floats truncate; non-numeric kinds read 0).</summary>
    public long AsInt => Kind == EventValueKind.Float ? (long)_x : IsNumeric ? _i : 0;
    /// <summary>The value as a float.</summary>
    public float AsFloat => Kind == EventValueKind.Float ? _x : IsNumeric ? _i : 0f;
    /// <summary>The value as a bool (non-zero is true).</summary>
    public bool AsBool => Kind == EventValueKind.Float ? _x != 0f : IsNumeric && _i != 0;
    /// <summary>The string, or empty for any other kind.</summary>
    public string AsString => _s ?? "";
    /// <summary>The vector, or zero for any other kind.</summary>
    public Vec3 AsVec3 => Kind == EventValueKind.Vec3 ? new Vec3(_x, _y, _z) : Vec3.Zero;
    /// <summary>The entity handle, or 0.</summary>
    public int AsEntity => (int)AsInt;
    private bool IsNumeric => Kind is EventValueKind.Int or EventValueKind.Float or EventValueKind.Bool or EventValueKind.Entity;
}

/// <summary>The event being delivered: a snapshot, safe to keep after the handler returns.</summary>
public sealed class EventData
{
    /// <summary>Event name.</summary>
    public string Name { get; init; } = "";
    /// <summary>Entity that raised it, or 0.</summary>
    public int Sender { get; init; }
    /// <summary>Entity it was addressed to, or 0 for a broadcast.</summary>
    public int Target { get; init; }
    /// <summary>Payload in push order.</summary>
    public IReadOnlyList<EventValue> Args { get; init; } = Array.Empty<EventValue>();

    private EventValue At(int i) => (uint)i < (uint)Args.Count ? Args[i] : default;
    /// <summary>Argument <paramref name="i"/> as an integer; 0 when absent.</summary>
    public long GetInt(int i) => At(i).AsInt;
    /// <summary>Argument <paramref name="i"/> as a float; 0 when absent.</summary>
    public float GetFloat(int i) => At(i).AsFloat;
    /// <summary>Argument <paramref name="i"/> as a bool; false when absent.</summary>
    public bool GetBool(int i) => At(i).AsBool;
    /// <summary>Argument <paramref name="i"/> as a string; empty when absent.</summary>
    public string GetString(int i) => At(i).AsString;
    /// <summary>Argument <paramref name="i"/> as a vector; zero when absent.</summary>
    public Vec3 GetVec3(int i) => At(i).AsVec3;
    /// <summary>Argument <paramref name="i"/> as an entity; <see cref="Entity.None"/> when absent.</summary>
    public Entity GetEntity(int i) => new Entity(At(i).AsEntity);
}

/// <summary>A subscription id. 0 is invalid.</summary>
public readonly struct EventSubscription : IEquatable<EventSubscription>
{
    /// <summary>The raw id.</summary>
    public int Id { get; }
    /// <summary>Wraps a raw id.</summary>
    public EventSubscription(int id) => Id = id;
    /// <summary>True for a non-zero id.</summary>
    public bool IsValid => Id != 0;
    /// <inheritdoc/>
    public bool Equals(EventSubscription other) => Id == other.Id;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is EventSubscription s && Equals(s);
    /// <inheritdoc/>
    public override int GetHashCode() => Id;
}

/// <summary>Named events with typed payloads. Single game thread only.</summary>
/// <remarks>
/// <see cref="Dispatch"/> delivers now; <see cref="Post"/> queues until <see cref="Flush"/>, which
/// the host calls at a frame-safe point. Handlers may subscribe, unsubscribe, dispatch and post;
/// a handler subscribed during a dispatch first hears the NEXT event.
/// </remarks>
public static class Events
{
    private sealed class Sub
    {
        public Func<EventData, bool> Handler = null!;
        public int Id;
        public int Owner;
    }

    private static readonly Dictionary<long, Sub> s_subs = new();
    private static readonly EventDispatchFn s_dispatch = OnDispatch;   // pinned for the process lifetime
    private static long s_nextToken = 1;
    private static bool s_installed;

    private static void EnsureInstalled()
    {
        if (s_installed) return;
        FwTE.aver_fw_set_event_dispatch(s_dispatch, IntPtr.Zero);
        s_installed = true;
    }

    private static int OnDispatch(int subscription, long token, IntPtr user)
    {
        try
        {
            if (!s_subs.TryGetValue(token, out Sub? sub)) return 0;
            return sub.Handler(ReadCurrent()) ? 1 : 0;
        }
        catch (Exception ex)
        {
            Log.Error($"[Events] handler threw: {ex.GetType().Name}: {ex.Message}");
            return 0;
        }
    }

    /// <summary>Snapshots the event being delivered. Null outside a handler.</summary>
    public static EventData? Current => FwTE.aver_fw_event_current_valid() == 0 ? null : ReadCurrent();

    internal static EventData ReadCurrent()
    {
        int n = FwTE.aver_fw_event_current_arg_count();
        var args = new EventValue[n];
        var xyz = new float[3];
        for (int i = 0; i < n; ++i)
        {
            var kind = (EventValueKind)FwTE.aver_fw_event_current_arg_type(i);
            args[i] = kind switch
            {
                EventValueKind.Int => new EventValue(kind, FwTE.aver_fw_event_current_arg_int(i), 0, 0, 0, null),
                EventValueKind.Entity => new EventValue(kind, FwTE.aver_fw_event_current_arg_entity(i), 0, 0, 0, null),
                EventValueKind.Bool => new EventValue(kind, FwTE.aver_fw_event_current_arg_bool(i), 0, 0, 0, null),
                EventValueKind.Float => new EventValue(kind, 0, FwTE.aver_fw_event_current_arg_float(i), 0, 0, null),
                EventValueKind.String => new EventValue(kind, 0, 0, 0, 0,
                    Marshal.PtrToStringUTF8(FwTE.aver_fw_event_current_arg_string(i)) ?? ""),
                EventValueKind.Vec3 when FwTE.aver_fw_event_current_arg_vec3(i, xyz) != 0 =>
                    new EventValue(kind, 0, xyz[0], xyz[1], xyz[2], null),
                _ => default,
            };
        }
        return new EventData
        {
            Name = Marshal.PtrToStringUTF8(FwTE.aver_fw_event_current_name()) ?? "",
            Sender = FwTE.aver_fw_event_current_sender(),
            Target = FwTE.aver_fw_event_current_target(),
            Args = args,
        };
    }

    /// <summary>Subscribes. Higher <paramref name="priority"/> runs first. <paramref name="senderFilter"/> != 0
    /// limits delivery to that sender. <paramref name="owner"/> is the <see cref="UnsubscribeOwner"/> key and
    /// the target filter: an event dispatched with a target reaches only subscribers whose owner equals it.
    /// The handler returns true to consume the event and stop later subscribers.</summary>
    public static EventSubscription SubscribeConsuming(string name, Func<EventData, bool> handler,
                                                       int owner = 0, int priority = 0, int senderFilter = 0)
    {
        ArgumentNullException.ThrowIfNull(handler);
        if (string.IsNullOrEmpty(name)) return default;
        EnsureInstalled();
        long token = s_nextToken++;
        var sub = new Sub { Handler = handler, Owner = owner };
        s_subs[token] = sub;
        int id = FwTE.aver_fw_event_subscribe(name, owner, priority, senderFilter, token);
        if (id == 0) { s_subs.Remove(token); return default; }
        sub.Id = id;
        return new EventSubscription(id);
    }

    /// <summary>Subscribes a handler that never consumes.</summary>
    public static EventSubscription Subscribe(string name, Action<EventData> handler,
                                              int owner = 0, int priority = 0, int senderFilter = 0)
    {
        ArgumentNullException.ThrowIfNull(handler);
        return SubscribeConsuming(name, e => { handler(e); return false; }, owner, priority, senderFilter);
    }

    /// <summary>Removes a subscription. Safe inside a handler, including the handler's own.</summary>
    public static bool Unsubscribe(EventSubscription s)
    {
        bool was = FwTE.aver_fw_event_unsubscribe(s.Id) != 0;
        if (was)
            foreach (long t in s_subs.Where(kv => kv.Value.Id == s.Id).Select(kv => kv.Key).ToList())
                s_subs.Remove(t);
        return was;
    }

    /// <summary>Removes every subscription made with <paramref name="owner"/>. Owner 0 is a no-op.</summary>
    public static int UnsubscribeOwner(int owner)
    {
        int n = FwTE.aver_fw_event_unsubscribe_owner(owner);
        if (n > 0)
            foreach (long t in s_subs.Where(kv => kv.Value.Owner == owner).Select(kv => kv.Key).ToList())
                s_subs.Remove(t);
        return n;
    }

    /// <summary>Number of live subscribers to <paramref name="name"/>.</summary>
    public static int ListenerCount(string name) => FwTE.aver_fw_event_listener_count(name);

    /// <summary>Delivers now. Returns how many handlers ran. Past 8 nested dispatches the event is queued instead.</summary>
    /// <param name="args">int, long, float, double, bool, string, <see cref="Vec3"/>, <see cref="Entity"/> or <see cref="EventValue"/>.</param>
    public static int Dispatch(string name, int sender = 0, int target = 0, params object[] args)
    {
        Stage(args);
        return FwTE.aver_fw_event_dispatch(name, sender, target);
    }

    /// <summary>Queues for delivery at the next <see cref="Flush"/>.</summary>
    public static bool Post(string name, int sender = 0, int target = 0, params object[] args)
    {
        Stage(args);
        return FwTE.aver_fw_event_post(name, sender, target) > 0;
    }

    /// <summary>Host call at the frame-safe point. Returns the number of events delivered.</summary>
    public static int Flush() => FwTE.aver_fw_events_flush();
    /// <summary>Events waiting for the next flush.</summary>
    public static int Pending => FwTE.aver_fw_events_pending();
    /// <summary>How many dispatches were demoted to the queue by the nesting limit.</summary>
    public static int Overflowed => FwTE.aver_fw_events_overflowed();
    /// <summary>Drops queued events, keeping subscriptions.</summary>
    public static void ClearPending() => FwTE.aver_fw_events_clear_pending();

    /// <summary>Drops queued events and every subscription.</summary>
    public static void ClearAll()
    {
        FwTE.aver_fw_events_clear();
        s_subs.Clear();
    }

    /// <summary>Detaches from the native bus. Call before the scripting load context is unloaded.</summary>
    public static void Shutdown()
    {
        if (!s_installed) return;
        FwTE.aver_fw_events_clear();
        FwTE.aver_fw_set_event_dispatch(null, IntPtr.Zero);
        s_subs.Clear();
        s_installed = false;
    }

    private static void Stage(object[] args)
    {
        FwTE.aver_fw_event_payload_clear();
        foreach (object a in args)
        {
            switch (a)
            {
                case int i: FwTE.aver_fw_event_push_int(i); break;
                case long l: FwTE.aver_fw_event_push_int(l); break;
                case float f: FwTE.aver_fw_event_push_float(f); break;
                case double d: FwTE.aver_fw_event_push_float((float)d); break;
                case bool b: FwTE.aver_fw_event_push_bool(b ? 1 : 0); break;
                case string s: FwTE.aver_fw_event_push_string(s); break;
                case Vec3 v: FwTE.aver_fw_event_push_vec3(v.X, v.Y, v.Z); break;
                case Entity e: FwTE.aver_fw_event_push_entity(e.Handle); break;
                case EventValue ev: PushValue(ev); break;
                default: Log.Warn($"[Events] unsupported payload type {a?.GetType().Name ?? "null"} skipped"); break;
            }
        }
    }

    private static void PushValue(EventValue v)
    {
        switch (v.Kind)
        {
            case EventValueKind.Int: FwTE.aver_fw_event_push_int(v.AsInt); break;
            case EventValueKind.Float: FwTE.aver_fw_event_push_float(v.AsFloat); break;
            case EventValueKind.Bool: FwTE.aver_fw_event_push_bool(v.AsBool ? 1 : 0); break;
            case EventValueKind.String: FwTE.aver_fw_event_push_string(v.AsString); break;
            case EventValueKind.Vec3: FwTE.aver_fw_event_push_vec3(v.AsVec3.X, v.AsVec3.Y, v.AsVec3.Z); break;
            case EventValueKind.Entity: FwTE.aver_fw_event_push_entity(v.AsEntity); break;
        }
    }
}
