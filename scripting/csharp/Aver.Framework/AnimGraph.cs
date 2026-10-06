// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Driving an animation state machine (.ocasm) from a script: the CAnimGraph surface on Entity.
//
// No new P/Invoke. Parameters travel as (name hash, value) slots on the CAnimGraph component, written
// over the generic scene field ABI exactly as SetControlRig writes its fields. CAnimGraph is registered
// at RUNTIME like CControlRig, so a host that never registered it gets false, not a throw.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private const int AnimGraphSlots = 16;
    private const int AnimGraphPausedBit = 1;

    /// <summary>Runs the state machine at <paramref name="machineAsset"/> on this entity, adding the
    /// component if needed. The entity needs a skeleton (<see cref="SetSkeleton"/>) to be posed.
    ///
    /// <para><b>playRate is written every call</b> for the reason SetControlRig writes weight: the
    /// component's storage arrives zero-filled, and although the system reads 0 as 1, an explicit write
    /// keeps what a script reads back honest.</para></summary>
    public bool SetAnimGraph(string machineAsset, float playRate = 1.0f)
    {
        if (!HasComponent("CAnimGraph") && !AddComponent("CAnimGraph")) return false;
        SetFloat("CAnimGraph.playRate", playRate);
        return SetInt64("CAnimGraph.machine", Assets.ObjectIdOf(machineAsset));
    }

    /// <summary>Writes a machine parameter by name. Finds the slot already holding the name's hash, or
    /// the first free one; false when all sixteen slots belong to other names or the entity has no
    /// graph. A float parameter keeps the value; an int is rounded and a bool is non-zero by the
    /// system, which knows each parameter's declared type.</summary>
    public bool SetAnimParam(string name, float value)
    {
        if (!HasComponent("CAnimGraph")) return false;
        long hash = Assets.ObjectIdOf(name);
        int free = -1;
        for (int i = 0; i < AnimGraphSlots; i++)
        {
            long h = GetInt64("CAnimGraph.paramHash" + i);
            if (h == hash) { free = i; break; }
            if (h == 0 && free < 0) free = i;
        }
        if (free < 0) return false;
        // Value first, hash second: the system reads a slot only once its hash is non-zero, so a
        // half-written slot is never taken for a different parameter's value.
        SetFloat("CAnimGraph.paramValue" + free, value);
        return SetInt64("CAnimGraph.paramHash" + free, hash);
    }

    /// <summary>Sets a bool machine parameter.</summary>
    public bool SetAnimParam(string name, bool value) => SetAnimParam(name, value ? 1.0f : 0.0f);

    /// <summary>Sets an int machine parameter.</summary>
    public bool SetAnimParam(string name, int value) => SetAnimParam(name, (float)value);

    /// <summary>Fires a trigger parameter. The system takes it on the next tick and clears it.</summary>
    public bool TriggerAnim(string name) => SetAnimParam(name, 1.0f);

    /// <summary>Reads a machine parameter back, or null when the name was never written.</summary>
    public float? GetAnimParam(string name)
    {
        if (!HasComponent("CAnimGraph")) return null;
        long hash = Assets.ObjectIdOf(name);
        for (int i = 0; i < AnimGraphSlots; i++)
            if (GetInt64("CAnimGraph.paramHash" + i) == hash) return GetFloat("CAnimGraph.paramValue" + i);
        return null;
    }

    /// <summary>True while the machine's active leaf state has this name. Compared by hash, since the
    /// component cannot carry a string.</summary>
    public bool IsInAnimState(string stateName) =>
        HasComponent("CAnimGraph") && GetInt64("CAnimGraph.activeState") == Assets.ObjectIdOf(stateName);

    /// <summary>Cumulative normalised time of the active leaf state: 1.0 is one pass through its clip
    /// (or blend-space cycle), and it keeps counting across loops.</summary>
    public float AnimStateTime => HasComponent("CAnimGraph") ? GetFloat("CAnimGraph.stateTime") : 0.0f;

    /// <summary>Playback rate of the machine: 1 is normal, 0 is read as 1 (use
    /// <see cref="PauseAnimGraph"/> to stop).</summary>
    public float AnimGraphRate
    {
        get => GetFloat("CAnimGraph.playRate");
        set => SetFloat("CAnimGraph.playRate", value);
    }

    /// <summary>Freezes the machine's clock and transitions; the pose holds.</summary>
    public bool PauseAnimGraph() => SetInt("CAnimGraph.flags", GetInt("CAnimGraph.flags") | AnimGraphPausedBit);

    /// <summary>Continues a paused machine.</summary>
    public bool ResumeAnimGraph() => SetInt("CAnimGraph.flags", GetInt("CAnimGraph.flags") & ~AnimGraphPausedBit);
}

/// <summary>The graph-node surface for the animation state machine nodes: the same Entity calls with
/// the int-entity / primitive-only signatures GraphCompiler's emitted IL needs. Kept in this file and
/// forwarded from GraphInterop by the integration step; see docs/ANIM_BLEND_STATE.md section 7.</summary>
internal static class AnimGraphInterop
{
    internal static bool SetAnimGraphForGraph(int entity, string machineAsset, float playRate) =>
        new Entity(entity).SetAnimGraph(machineAsset, playRate);

    internal static bool SetAnimParamFloatForGraph(int entity, string name, float value) =>
        new Entity(entity).SetAnimParam(name, value);

    internal static bool SetAnimParamBoolForGraph(int entity, string name, bool value) =>
        new Entity(entity).SetAnimParam(name, value);

    internal static bool TriggerAnimForGraph(int entity, string name) =>
        new Entity(entity).TriggerAnim(name);

    internal static bool IsInAnimStateForGraph(int entity, string stateName) =>
        new Entity(entity).IsInAnimState(stateName);

    internal static float AnimStateTimeForGraph(int entity) =>
        new Entity(entity).AnimStateTime;
}
