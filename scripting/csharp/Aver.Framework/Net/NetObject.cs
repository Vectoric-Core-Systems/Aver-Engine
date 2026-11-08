// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// NetObject: the unit of replication. A subclass marks fields or properties [Replicated] and methods [Rpc]; a NetSession keeps them
// converged between the server and its clients.

using System.Reflection;
using Aver.Scene;

namespace Aver.Framework.Net;

/// <summary>Where a [Rpc] method runs.</summary>
public enum RpcTarget
{
    /// <summary>On the server. A client may call it only on an object it owns; the server drops the call otherwise.</summary>
    Server = 0,
    /// <summary>On the client that owns the object (on the server itself when the server owns it). Only the server may call it.</summary>
    Client = 1,
    /// <summary>On the server and on every client the object is spawned on. Only the server may call it.</summary>
    Multicast = 2,
}

/// <summary>Marks a field or property of a <see cref="NetObject"/> as server-to-client state. The server compares the value each replication
/// pass and sends only what changed. The type must be one <see cref="NetCodec"/> supports.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ReplicatedAttribute : Attribute
{
    /// <summary>Sent reliably (default). When false the value rides the unreliable channel and is resent in full every
    /// <see cref="NetSession.UnreliableResyncMs"/>, so a lost packet is only a delay.</summary>
    public bool Reliable { get; init; } = true;
    /// <summary>Only the owning client (and the server) gets the value.</summary>
    public bool OwnerOnly { get; init; }
    /// <summary>Name of a parameterless method (or one taking the previous value) called on a client after a received change.</summary>
    public string? OnChanged { get; init; }
}

/// <summary>Marks a method of a <see cref="NetObject"/> as callable across the network with <see cref="NetObject.CallRpc"/>.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class RpcAttribute : Attribute
{
    public RpcAttribute(RpcTarget target) => Target = target;
    public RpcTarget Target { get; }
    /// <summary>Acknowledged and ordered with spawns and property changes (default). When false the call is dropped if the packet is lost.</summary>
    public bool Reliable { get; init; } = true;
}

/// <summary>Registers a <see cref="NetObject"/> subclass under the name that crosses the wire. It needs a public parameterless constructor.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class NetTypeAttribute : Attribute
{
    public NetTypeAttribute(string name) => Name = name;
    public string Name { get; }
}

internal sealed class NetPropInfo
{
    public string Name = "";
    public Type Type = typeof(int);
    public Func<object, object?> Get = null!;
    public Action<object, object?> Set = null!;
    public bool Reliable, OwnerOnly;
    public MethodInfo? OnChanged;
}

internal sealed class NetRpcInfo
{
    public string Name = "";
    public MethodInfo Method = null!;
    public RpcTarget Target;
    public bool Reliable;
    public Type[] ParamTypes = Type.EmptyTypes;
}

internal sealed class NetTypeInfo
{
    public Type Type = null!;
    public string Name = "";
    public NetPropInfo[] Props = Array.Empty<NetPropInfo>();
    public Dictionary<string, NetRpcInfo> Rpcs = new();
    public bool HasUnreliable;
}

/// <summary>The type registry: names to classes, and the per-class property and RPC tables built once by reflection.</summary>
public static class NetTypes
{
    private static readonly Dictionary<string, Type> s_byName = new();
    private static readonly Dictionary<Type, string> s_names = new();
    private static readonly Dictionary<Type, NetTypeInfo> s_info = new();

    public static void Register<T>(string? name = null) where T : NetObject, new() => Register(typeof(T), name);

    public static void Register(Type type, string? name = null)
    {
        if (!typeof(NetObject).IsAssignableFrom(type) || type.IsAbstract || type.GetConstructor(Type.EmptyTypes) is null)
            throw new ArgumentException(type.Name + " must be a concrete NetObject with a public parameterless constructor");
        name ??= type.GetCustomAttribute<NetTypeAttribute>()?.Name ?? type.Name;
        lock (s_byName) { s_byName[name] = type; s_names[type] = name; }
    }

    /// <summary>Registers every [NetType] class in <paramref name="assembly"/>.</summary>
    public static void RegisterAssembly(Assembly assembly)
    {
        foreach (Type t in assembly.GetTypes())
            if (t.GetCustomAttribute<NetTypeAttribute>() is { } a && typeof(NetObject).IsAssignableFrom(t) && !t.IsAbstract) Register(t, a.Name);
    }

    internal static string? NameOf(Type t) { lock (s_byName) return s_names.GetValueOrDefault(t); }

    internal static NetObject? Create(string name)
    {
        Type? t;
        lock (s_byName) s_byName.TryGetValue(name, out t);
        return t is null ? null : (NetObject)Activator.CreateInstance(t)!;
    }

    internal static NetTypeInfo Info(Type type)
    {
        lock (s_info)
        {
            if (s_info.TryGetValue(type, out NetTypeInfo? known)) return known;
            var info = new NetTypeInfo { Type = type, Name = NameOf(type) ?? type.Name };
            var props = new List<NetPropInfo>();
            const BindingFlags F = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
            for (Type? t = type; t is not null && t != typeof(object); t = t.BaseType)
            {
                foreach (FieldInfo f in t.GetFields(F))
                    if (f.GetCustomAttribute<ReplicatedAttribute>() is { } a) props.Add(MakeProp(type, f.Name, f.FieldType, f.GetValue, f.SetValue, a));
                foreach (PropertyInfo p in t.GetProperties(F))
                    if (p.GetCustomAttribute<ReplicatedAttribute>() is { } a && p.GetMethod is not null && p.SetMethod is not null)
                        props.Add(MakeProp(type, p.Name, p.PropertyType, p.GetValue, (o, v) => p.SetValue(o, v), a));
                foreach (MethodInfo m in t.GetMethods(F))
                    if (m.GetCustomAttribute<RpcAttribute>() is { } r && !info.Rpcs.ContainsKey(m.Name))
                    {
                        Type[] ps = m.GetParameters().Select(x => x.ParameterType).ToArray();
                        if (ps.Any(x => !NetCodec.IsSupported(x))) throw new InvalidOperationException($"[Rpc] {type.Name}.{m.Name} has a parameter type that cannot cross the wire");
                        info.Rpcs[m.Name] = new NetRpcInfo { Name = m.Name, Method = m, Target = r.Target, Reliable = r.Reliable, ParamTypes = ps };
                    }
            }
            props.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));
            info.Props = props.ToArray();
            info.HasUnreliable = props.Any(p => !p.Reliable);
            s_info[type] = info;
            return info;
        }
    }

    private static NetPropInfo MakeProp(Type owner, string name, Type type, Func<object?, object?> get, Action<object?, object?> set, ReplicatedAttribute a)
    {
        if (!NetCodec.IsSupported(type)) throw new InvalidOperationException($"[Replicated] {owner.Name}.{name}: {type.Name} cannot cross the wire");
        MethodInfo? cb = a.OnChanged is null ? null
            : owner.GetMethod(a.OnChanged, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
              ?? throw new InvalidOperationException($"[Replicated] {owner.Name}.{name}: no method {a.OnChanged}");
        return new NetPropInfo
        {
            Name = name, Type = type, Get = o => get(o), Set = (o, v) => set(o, v),
            Reliable = a.Reliable, OwnerOnly = a.OwnerOnly, OnChanged = cb,
        };
    }
}

/// <summary>Per-connection bookkeeping for one object the connection has been told about.</summary>
internal sealed class NetSyncState
{
    public uint[] SentVersion = Array.Empty<uint>();
    public long LastUnreliableMs = long.MinValue;
}

/// <summary>An object a <see cref="NetSession"/> replicates. On the server it is the truth; on a client it is a proxy that receives
/// property changes and RPCs. Derive, mark state [Replicated] and methods [Rpc], register with <see cref="NetTypes"/>, and spawn it with
/// <see cref="NetSession.Spawn"/>.</summary>
public abstract class NetObject
{
    internal NetTypeInfo Info = null!;
    internal byte[]?[] Current = Array.Empty<byte[]?>();
    internal uint[] Version = Array.Empty<uint>();

    /// <summary>The id both sides know it by (0 until spawned).</summary>
    public uint NetId { get; internal set; }
    public NetSession? Session { get; internal set; }
    /// <summary>The connection that owns it: 0 is the server (nobody), otherwise a <see cref="NetConnection.Id"/>.</summary>
    public uint OwnerId { get; internal set; }

    /// <summary>True on the server (and on an object with no session).</summary>
    public bool HasAuthority => Session is null || Session.IsServer;
    /// <summary>True where this process is the owner: the owning client, or the server for an object it owns itself.</summary>
    public bool IsOwner => Session is not null && OwnerId == Session.LocalConnectionId;
    /// <summary>Whether the object stays in a client's world no matter how far away it is.</summary>
    public virtual bool AlwaysRelevant => false;
    /// <summary>Where the object is, for distance relevance (see <see cref="NetRelevance.Distance"/>).</summary>
    public virtual Vec3 NetPosition => Vec3.Zero;

    /// <summary>Called on the server after Spawn, and on a client after the spawn message and its initial state arrive.</summary>
    protected virtual void OnNetSpawn() { }
    /// <summary>Called when the object is removed: destroyed, out of relevance on a client, or the session ended.</summary>
    protected virtual void OnNetDestroy() { }
    /// <summary>Called on every side when the owner changes.</summary>
    protected virtual void OnOwnerChanged(uint previous) { }

    internal void NotifySpawn() => OnNetSpawn();
    internal void NotifyDestroy() => OnNetDestroy();
    internal void NotifyOwner(uint previous) => OnOwnerChanged(previous);

    /// <summary>Calls the [Rpc] method <paramref name="method"/> where its target says: locally, over the network, or both.</summary>
    protected void CallRpc(string method, params object?[] args)
    {
        if (Session is null) throw new InvalidOperationException("the object is not in a session");
        Session.SendRpc(this, method, args);
    }
}

/// <summary>Ready-made relevance rules for <see cref="NetSession.RelevanceFilter"/>.</summary>
public static class NetRelevance
{
    /// <summary>An object is relevant to a connection when it is within <paramref name="radius"/> of the connection's
    /// <see cref="NetConnection.ViewPosition"/>, or always relevant, or owned by that connection.</summary>
    public static Func<NetObject, NetConnection, bool> Distance(float radius)
    {
        float r2 = radius * radius;
        return (o, c) =>
        {
            if (o.AlwaysRelevant || o.OwnerId == c.Id) return true;
            Vec3 d = o.NetPosition - c.ViewPosition;
            return d.X * d.X + d.Y * d.Y + d.Z * d.Z <= r2;
        };
    }
}
