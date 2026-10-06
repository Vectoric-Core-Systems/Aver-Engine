// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Scalar-signature seam the AN_SetTimer / AN_ClearTimer / AN_DispatchEvent / AN_OnEvent /
// AN_EventPayload nodes' compiled IL calls. Lives here, not in Aver.Framework, because a timer or an
// event reaches a graph through GraphEvents.Router, which Aver.Framework cannot see.

using Aver.Framework;
using Aver.Scripting;

namespace Aver.Graph;

/// <summary>Graph-facing timers and events. Wiring: docs/GAME_TIMERS_EVENTS.md.</summary>
public static class GraphTimerEvents
{
    /// <summary>The node type a graph uses for an event entry; its ENTRY name is the bus event name.</summary>
    public const string OnEventNodeType = "AN_OnEvent";

    private static readonly Dictionary<int, List<EventSubscription>> s_bound = new();

    /// <summary>AN_SetTimer: fires graph event <paramref name="eventName"/> on <paramref name="target"/>
    /// after <paramref name="delay"/> seconds, repeating when <paramref name="looping"/>. The timer is
    /// owned by the target, so unbinding the entity cancels it. Returns the handle, 0 if nothing was set.</summary>
    public static int SetTimerForGraph(int target, float delay, bool looping, string eventName)
    {
        if (target == 0 || string.IsNullOrEmpty(eventName))
        {
            Log.Warn($"[Graph] AN_SetTimer: not set (target {target}, event '{eventName}')");
            return 0;
        }
        TimerHandle h = Timers.Set(delay, () => GraphEvents.FireEventForGraph(target, eventName),
                                   repeat: looping, owner: target);
        return h.Value;
    }

    /// <summary>AN_ClearTimer: true if the timer was still scheduled.</summary>
    public static bool ClearTimerForGraph(int handle) => Timers.Clear(new TimerHandle(handle));

    /// <summary>AN_DispatchEvent: payload is [int, float]. Deferred to the frame's flush unless
    /// <paramref name="immediate"/>; a graph that dispatches mid-chain should normally defer.</summary>
    public static void DispatchEventForGraph(int sender, int target, int intValue, float floatValue,
                                             bool immediate, string eventName)
    {
        if (string.IsNullOrEmpty(eventName)) return;
        if (immediate) Events.Dispatch(eventName, sender, target, (long)intValue, floatValue);
        else Events.Post(eventName, sender, target, (long)intValue, floatValue);
    }

    /// <summary>AN_EventPayload: reads argument <paramref name="index"/> of the event whose AN_OnEvent
    /// chain is running. Everything reads 0 outside one or past the last argument.</summary>
    public static void ReadEventPayloadForGraph(int index, out int sender, out int target,
                                                out int intValue, out float floatValue, out bool boolValue)
    {
        EventData? e = Events.Current;
        sender = e?.Sender ?? 0;
        target = e?.Target ?? 0;
        intValue = e is null ? 0 : (int)e.GetInt(index);
        floatValue = e?.GetFloat(index) ?? 0f;
        boolValue = e?.GetBool(index) ?? false;
    }

    /// <summary>Bridge call when a graph instance binds to <paramref name="entity"/>: subscribes the
    /// entity to the bus event named by every AN_OnEvent ENTRY, so a matching event runs that entry.
    /// Replaces any earlier binding of the same entity (hot reload).</summary>
    public static void BindGraph(int entity, Graph graph)
    {
        ArgumentNullException.ThrowIfNull(graph);
        UnbindSubscriptions(entity);
        List<EventSubscription>? subs = null;
        foreach (var (nodeId, eventName) in graph.EntryPoints)
        {
            if (!graph.Nodes.TryGetValue(nodeId, out Node? node) ||
                !node.Type.Equals(OnEventNodeType, StringComparison.OrdinalIgnoreCase)) continue;
            string name = eventName;
            EventSubscription s = Events.Subscribe(name, _ => GraphEvents.FireEventForGraph(entity, name), owner: entity);
            if (!s.IsValid) continue;
            (subs ??= new()).Add(s);
        }
        if (subs != null) s_bound[entity] = subs;
    }

    /// <summary>Bridge call when the entity's graph is unbound or destroyed: drops its AN_OnEvent
    /// subscriptions and every timer it owns.</summary>
    public static void UnbindEntity(int entity)
    {
        UnbindSubscriptions(entity);
        Timers.ClearOwner(entity);
    }

    private static void UnbindSubscriptions(int entity)
    {
        if (!s_bound.Remove(entity, out List<EventSubscription>? old)) return;
        foreach (EventSubscription s in old) Events.Unsubscribe(s);
    }
}
