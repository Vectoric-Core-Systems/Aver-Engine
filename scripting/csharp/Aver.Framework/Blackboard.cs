// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Script-facing AI blackboard: typed per-entity keys, agent and team scope. Native side:
// framework_blackboard_abi.h; design: docs/BLACKBOARD_BT.md.

using System.Runtime.InteropServices;
using System.Text;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>P/Invoke into the blackboard relay of Aver.Framework. Internal: scripts use <see cref="Blackboard"/>.</summary>
/// <remarks>Declared in this assembly, like <c>Fw</c>, because only this assembly has the DllImport resolver
/// that routes "Aver.Framework" to the native DLL (NativeResolver.cs).</remarks>
internal static class FwBlackboard
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_bb_type(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_bb_get_bool(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, out int value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_get_int(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, out long value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_get_float(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, out float value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_get_vec3(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, float[] out3);
    [DllImport(Lib)] internal static extern int aver_fw_bb_get_string(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, byte[] buf, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_bb_get_entity(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, out int value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_bool(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, int value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_int(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, long value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_float(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, float value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_vec3(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_string(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_entity(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, int value);
    [DllImport(Lib)] internal static extern int aver_fw_bb_reset(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_bb_define(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, int type, int scope);
    [DllImport(Lib)] internal static extern int aver_fw_bb_set_team(int e, [MarshalAs(UnmanagedType.LPUTF8Str)] string team);
}

/// <summary>The type of a blackboard key. <see cref="None"/> means the key is not defined.</summary>
public enum BlackboardType { None = 0, Bool = 1, Int = 2, Float = 3, Vec3 = 4, String = 5, Entity = 6 }

/// <summary>Where a blackboard key lives: private to the agent, or on the board its whole team shares.</summary>
public enum BlackboardScope { Agent = 0, Shared = 1 }

/// <summary>One entity's AI blackboard (reach it with <c>entity.Blackboard</c>). Keys are typed; a write
/// to an undefined key defines it (agent scope), a write of the wrong type is refused (returns false),
/// and numeric types convert between themselves. Behaviour trees see every change: a decorator watching a
/// key can abort a running branch the moment it changes.</summary>
/// <remarks>A cheap value over the entity handle; nothing is cached. Every call returns false (or the
/// fallback) when the entity is dead or no AI provider is installed.</remarks>
public readonly struct Blackboard
{
    private readonly int _entity;

    internal Blackboard(int entity) => _entity = entity;

    /// <summary>The key's type, or <see cref="BlackboardType.None"/> when it is not defined.</summary>
    public BlackboardType TypeOf(string key) => (BlackboardType)FwBlackboard.aver_fw_bb_type(_entity, key);

    /// <summary>Whether the key is defined.</summary>
    public bool Has(string key) => TypeOf(key) != BlackboardType.None;

    /// <summary>Defines a key explicitly (a behaviour tree's own schema defines its keys already).</summary>
    public bool Define(string key, BlackboardType type, BlackboardScope scope = BlackboardScope.Agent)
        => FwBlackboard.aver_fw_bb_define(_entity, key, (int)type, (int)scope) != 0;

    /// <summary>Restores a key to its default value.</summary>
    public bool Reset(string key) => FwBlackboard.aver_fw_bb_reset(_entity, key) != 0;

    /// <summary>Puts this entity on a team: its Shared keys then live on that team's board, visible to
    /// every teammate. An empty name is the default team.</summary>
    public bool SetTeam(string team) => FwBlackboard.aver_fw_bb_set_team(_entity, team) != 0;

    public bool TryGetBool(string key, out bool value)
    {
        bool ok = FwBlackboard.aver_fw_bb_get_bool(_entity, key, out int v) != 0;
        value = ok && v != 0;
        return ok;
    }
    public bool GetBool(string key, bool fallback = false) => TryGetBool(key, out bool v) ? v : fallback;
    public bool SetBool(string key, bool value) => FwBlackboard.aver_fw_bb_set_bool(_entity, key, value ? 1 : 0) != 0;

    public bool TryGetInt(string key, out long value)
    {
        bool ok = FwBlackboard.aver_fw_bb_get_int(_entity, key, out long v) != 0;
        value = ok ? v : 0;
        return ok;
    }
    public long GetInt(string key, long fallback = 0) => TryGetInt(key, out long v) ? v : fallback;
    public bool SetInt(string key, long value) => FwBlackboard.aver_fw_bb_set_int(_entity, key, value) != 0;

    public bool TryGetFloat(string key, out float value)
    {
        bool ok = FwBlackboard.aver_fw_bb_get_float(_entity, key, out float v) != 0;
        value = ok ? v : 0f;
        return ok;
    }
    public float GetFloat(string key, float fallback = 0f) => TryGetFloat(key, out float v) ? v : fallback;
    public bool SetFloat(string key, float value) => FwBlackboard.aver_fw_bb_set_float(_entity, key, value) != 0;

    public bool TryGetVec3(string key, out Vec3 value)
    {
        var buf = new float[3];
        bool ok = FwBlackboard.aver_fw_bb_get_vec3(_entity, key, buf) != 0;
        value = ok ? new Vec3(buf[0], buf[1], buf[2]) : Vec3.Zero;
        return ok;
    }
    public Vec3 GetVec3(string key, Vec3 fallback = default) => TryGetVec3(key, out Vec3 v) ? v : fallback;
    public bool SetVec3(string key, Vec3 value) => FwBlackboard.aver_fw_bb_set_vec3(_entity, key, value.X, value.Y, value.Z) != 0;

    public bool TryGetString(string key, out string value)
    {
        var buf = new byte[256];
        bool ok = FwBlackboard.aver_fw_bb_get_string(_entity, key, buf, buf.Length) != 0;
        int n = Array.IndexOf(buf, (byte)0);
        value = ok ? Encoding.UTF8.GetString(buf, 0, n < 0 ? buf.Length : n) : "";
        return ok;
    }
    public string GetString(string key, string fallback = "") => TryGetString(key, out string v) ? v : fallback;
    public bool SetString(string key, string value) => FwBlackboard.aver_fw_bb_set_string(_entity, key, value ?? "") != 0;

    public bool TryGetEntity(string key, out Entity value)
    {
        bool ok = FwBlackboard.aver_fw_bb_get_entity(_entity, key, out int v) != 0;
        value = ok ? new Entity(v) : Entity.None;
        return ok;
    }
    public Entity GetEntity(string key) => TryGetEntity(key, out Entity v) ? v : Entity.None;
    public bool SetEntity(string key, Entity value) => FwBlackboard.aver_fw_bb_set_entity(_entity, key, value.Handle) != 0;
}

public readonly partial struct Entity
{
    /// <summary>This entity's AI blackboard (see <see cref="Blackboard"/>).</summary>
    public Blackboard Blackboard => new(Handle);
}

/// <summary>What the Aver Node blackboard nodes call (GetBlackboardFloat and friends). Reached by reflection
/// from Aver.Graph.GraphCompiler, like <c>GraphInterop</c>. Gets return false (with a zero value) when the
/// key is missing or of an unconvertible type; sets return whether the write was accepted.</summary>
internal static class BlackboardGraph
{
    internal static bool GetFloatForGraph(int entity, string key, out float value)
    {
        value = 0f;
        return FwBlackboard.aver_fw_bb_get_float(entity, key, out value) != 0;
    }

    internal static bool GetIntForGraph(int entity, string key, out int value)
    {
        value = 0;
        if (FwBlackboard.aver_fw_bb_get_int(entity, key, out long v) == 0) return false;
        value = (int)Math.Clamp(v, int.MinValue, int.MaxValue);
        return true;
    }

    internal static bool GetBoolForGraph(int entity, string key, out bool value)
    {
        value = false;
        if (FwBlackboard.aver_fw_bb_get_bool(entity, key, out int v) == 0) return false;
        value = v != 0;
        return true;
    }

    internal static bool GetVec3ForGraph(int entity, string key, out float x, out float y, out float z)
    {
        var buf = new float[3];
        bool ok = FwBlackboard.aver_fw_bb_get_vec3(entity, key, buf) != 0;
        x = ok ? buf[0] : 0f; y = ok ? buf[1] : 0f; z = ok ? buf[2] : 0f;
        return ok;
    }

    /// <summary>For Entity-typed keys: the handle (0 when unset).</summary>
    internal static bool GetEntityForGraph(int entity, string key, out int value)
    {
        value = 0;
        return FwBlackboard.aver_fw_bb_get_entity(entity, key, out value) != 0;
    }

    internal static bool SetFloatForGraph(int entity, string key, float value) => FwBlackboard.aver_fw_bb_set_float(entity, key, value) != 0;
    internal static bool SetIntForGraph(int entity, string key, int value) => FwBlackboard.aver_fw_bb_set_int(entity, key, value) != 0;
    internal static bool SetBoolForGraph(int entity, string key, bool value) => FwBlackboard.aver_fw_bb_set_bool(entity, key, value ? 1 : 0) != 0;
    internal static bool SetVec3ForGraph(int entity, string key, float x, float y, float z) => FwBlackboard.aver_fw_bb_set_vec3(entity, key, x, y, z) != 0;
    internal static bool SetEntityForGraph(int entity, string key, int value) => FwBlackboard.aver_fw_bb_set_entity(entity, key, value) != 0;
}
