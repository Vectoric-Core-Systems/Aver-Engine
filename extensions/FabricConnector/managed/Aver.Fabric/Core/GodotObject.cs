// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GodotObject: the base of every Fabric type. Instance ids, properties and methods by name, metadata, signals.

using System.Reflection;
using Aver.Framework;
using Aver.Scripting;

namespace Aver.Fabric;

/// <summary>The root of Godot's class tree (GodotObject). It is an <see cref="AverActor"/> so a scene node that scripts
/// derive from can be bound by the scripting bridge; resources and other non-node objects derive from it too but are never bound.</summary>
public abstract partial class GodotObject : AverActor, IDisposable
{
    /// <summary>Flags for <see cref="Connect"/> (Godot's CONNECT_* values).</summary>
    [Flags]
    public enum ConnectFlags : uint
    {
        /// <summary>Call the handler when the current tick pass ends, not during the emit.</summary>
        Deferred = 1,
        /// <summary>Saved with the scene; a runtime connection has no use for it.</summary>
        Persist = 2,
        /// <summary>Disconnect after the first emit.</summary>
        OneShot = 4,
        /// <summary>Connecting the same callable again counts up; each Disconnect counts down.</summary>
        ReferenceCounted = 8,
    }

    /// <summary>The names of the signals a class declares itself, by the GodotSharp convention.</summary>
    public class SignalName { }

    /// <summary>The names of the methods a class declares, by the GodotSharp convention (the converter generates one per script class).</summary>
    public class MethodName { }

    /// <summary>The names of the fields and properties a class declares, by the GodotSharp convention.</summary>
    public class PropertyName { }

    /// <summary>The notification numbers scripts compare <c>what</c> against in <c>_Notification</c>.</summary>
    public const int NotificationPostinitialize = 0;
    public const int NotificationPredelete = 1;
    public const int NotificationExtensionReloaded = 2;

    // ---- identity ----

    private static readonly Dictionary<ulong, WeakReference<GodotObject>> s_byId = new();
    private static ulong s_nextId = 0x0100_0000_0001UL;
    private ulong _id;
    private bool _freed;
    private bool _queuedForDeletion;

    /// <summary>True until the object is freed.</summary>
    internal bool IsAlive => !_freed;

    /// <summary>The object's id, stable for its life and never reused. Assigned on first use.</summary>
    public ulong GetInstanceId()
    {
        if (_id == 0)
        {
            lock (s_byId)
            {
                _id = s_nextId++;
                s_byId[_id] = new WeakReference<GodotObject>(this);
            }
        }
        return _id;
    }

    /// <summary>The live object with this id, or null.</summary>
    public static GodotObject? InstanceFromId(ulong instanceId)
    {
        lock (s_byId)
        {
            if (s_byId.TryGetValue(instanceId, out var wr) && wr.TryGetTarget(out GodotObject? o) && o.IsAlive) return o;
            s_byId.Remove(instanceId);
            return null;
        }
    }

    /// <summary>True when <paramref name="instance"/> is not null and has not been freed.</summary>
    public static bool IsInstanceValid(GodotObject? instance) => instance is { IsAlive: true };

    /// <summary>True once the object is queued to be freed at the end of the frame.</summary>
    public bool IsQueuedForDeletion() => _queuedForDeletion;

    internal void MarkQueuedForDeletion() => _queuedForDeletion = true;

    /// <summary>Frees the object now. Using it afterwards is an error; <see cref="IsInstanceValid"/> says false. Nodes override this.</summary>
    public virtual void Free()
    {
        if (_freed) return;
        Notification(NotificationPredelete);
        _freed = true;
        _conns = null;
        lock (s_byId) { if (_id != 0) s_byId.Remove(_id); }
    }

    // Marks the object freed without the destroy work (the engine already destroyed its entity).
    internal void MarkFreed()
    {
        if (_freed) return;
        _freed = true;
        _conns = null;
        lock (s_byId) { if (_id != 0) s_byId.Remove(_id); }
    }

    public void Dispose() { Free(); GC.SuppressFinalize(this); }

    // A memberwise copy shares this object's id and connections; give it its own.
    internal void ResetIdentity()
    {
        _id = 0; _freed = false; _queuedForDeletion = false; _conns = null;
        if (_meta != null) _meta = new Dictionary<string, Variant>(_meta);
    }

    /// <summary>The Godot class name: the nearest Aver.Fabric type at or above this object's type ("Node3D" for a script that extends it).</summary>
    public virtual string GetClass()
    {
        for (System.Type? t = GetType(); t != null; t = t.BaseType)
            if (t.Assembly == typeof(GodotObject).Assembly) return t.Name;
        return GetType().Name;
    }

    /// <summary>True when the object is, or derives from, the class of that name (native Godot class names and script class names).</summary>
    public virtual bool IsClass(string @class)
    {
        for (System.Type? t = GetType(); t != null; t = t.BaseType)
            if (t.Name == @class) return true;
        return false;
    }

    public override string ToString() => $"<{GetClass()}#{GetInstanceId()}>";

    /// <summary>Sends a notification to the script's <c>_Notification</c>. Engine-driven ones (ready, process, parented ...) arrive the same way.</summary>
    public void Notification(int what, bool reversed = false) => _Notification(what);

    /// <summary>Overridden by scripts to receive notifications. The base does nothing.</summary>
    public virtual void _Notification(int what) { }

    // ---- properties and methods by name ----

    /// <summary>Sets a property, a field or an exported variable by its Godot name.</summary>
    public void Set(StringName property, Variant value)
    {
        string name = property.ToString();
        int colon = name.IndexOf(':');
        if (colon > 0)
        {
            // "position:x": read the member, change one component, write it back.
            string head = name[..colon], rest = name[(colon + 1)..];
            if (MemberAccess.TryGet(this, head, out Variant cur) && SubSet(cur, rest, value, out Variant updated)) { MemberAccess.TrySet(this, head, updated); return; }
        }
        if (!MemberAccess.TrySet(this, name, value) && !SetMeta2(name, value) && !TryDynamicSet(name, value))
            Log.Warn($"[Fabric] {GetClass()} has no property '{name}'");
    }

    // Members that live outside C#: a GDExtension peer answers for the methods and properties its native class registered.
    internal virtual bool TryDynamicCall(string method, Variant[] args, out Variant result) { result = default; return false; }
    internal virtual bool TryDynamicGet(string name, out Variant value) { value = default; return false; }
    internal virtual bool TryDynamicSet(string name, Variant value) => false;
    internal virtual bool HasDynamicMethod(string name) => false;

    private bool SetMeta2(string name, Variant value)
    {
        if (!name.StartsWith("metadata/", StringComparison.Ordinal)) return false;
        SetMeta(name["metadata/".Length..], value);
        return true;
    }

    private static bool SubSet(Variant cur, string comp, Variant value, out Variant updated)
    {
        updated = cur;
        float f = value.AsSingle();
        switch (cur.VariantType)
        {
            case Variant.Type.Vector3:
            {
                Vector3 v = cur.AsVector3();
                if (comp is "x" or "y" or "z") { v[comp[0] - 'x'] = f; updated = v; return true; }
                break;
            }
            case Variant.Type.Vector2:
            {
                Vector2 v = cur.AsVector2();
                if (comp is "x" or "y") { v[comp[0] - 'x'] = f; updated = v; return true; }
                break;
            }
            case Variant.Type.Color:
            {
                Color c = cur.AsColor();
                int i = comp switch { "r" => 0, "g" => 1, "b" => 2, "a" => 3, _ => -1 };
                if (i >= 0) { c[i] = f; updated = c; return true; }
                break;
            }
        }
        return false;
    }

    /// <summary>Reads a property, a field or an exported variable by its Godot name; nil when there is none.</summary>
    public Variant Get(StringName property)
    {
        string name = property.ToString();
        int colon = name.IndexOf(':');
        if (colon > 0)
        {
            Variant head = Get(name[..colon]);
            string comp = name[(colon + 1)..];
            return head.VariantType switch
            {
                Variant.Type.Vector3 when comp is "x" or "y" or "z" => (Variant)head.AsVector3()[comp[0] - 'x'],
                Variant.Type.Vector2 when comp is "x" or "y" => (Variant)head.AsVector2()[comp[0] - 'x'],
                Variant.Type.Color when comp is "r" or "g" or "b" or "a" => (Variant)head.AsColor()[comp switch { "r" => 0, "g" => 1, "b" => 2, _ => 3 }],
                _ => Variant.Nil,
            };
        }
        if (MemberAccess.TryGet(this, name, out Variant v)) return v;
        if (name.StartsWith("metadata/", StringComparison.Ordinal)) return GetMeta(name["metadata/".Length..]);
        if (TryDynamicGet(name, out Variant dynamicValue)) return dynamicValue;
        return default;
    }

    /// <summary>Sets a property at the end of the current tick pass.</summary>
    public void SetDeferred(StringName property, Variant value) => Deferred.Call(() => { if (IsAlive) Set(property, value); });

    /// <summary>Calls a method by its Godot name (or its C# name). Throws if there is none or it throws.</summary>
    public Variant Call(StringName method, params Variant[] args)
    {
        args ??= System.Array.Empty<Variant>();
        MethodInfo? m = MemberAccess.FindMethod(GetType(), method.ToString(), args.Length);
        if (m == null)
        {
            if (TryDynamicCall(method.ToString(), args, out Variant dynamicResult)) return dynamicResult;
            throw new MissingMethodException(GetType().Name, method.ToString());
        }
        return MemberAccess.Invoke(m.IsStatic ? null : this, m, args);
    }

    /// <summary>Calls a method when the current tick pass ends.</summary>
    public Variant CallDeferred(StringName method, params Variant[] args)
    {
        Deferred.Call(() =>
        {
            if (!IsAlive) return;
            try { Call(method, args); }
            catch (Exception ex) { Log.Error($"[Fabric] deferred call of {GetClass()}.{method} threw: {ex.GetType().Name}: {ex.Message}"); }
        });
        return default;
    }

    public bool HasMethod(StringName method) => MemberAccess.HasMethod(GetType(), method.ToString()) || HasDynamicMethod(method.ToString());

    /// <summary>The names of the properties and fields scripts can reach by name (exported or declared by a script class).</summary>
    public string[] GetExportedPropertyNames()
    {
        var names = new List<string>();
        for (System.Type? t = GetType(); t != null && t.Assembly != typeof(GodotObject).Assembly && t != typeof(object); t = t.BaseType)
        {
            foreach (MemberInfo m in t.GetMembers(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                if (m is FieldInfo or PropertyInfo && m.GetCustomAttribute<ExportAttribute>() != null) names.Add(m.Name);
        }
        return names.ToArray();
    }

    // ---- metadata ----

    private Dictionary<string, Variant>? _meta;

    public void SetMeta(StringName name, Variant value)
    {
        if (value.IsNil) { RemoveMeta(name); return; }
        (_meta ??= new())[name.ToString()] = value;
    }

    public Variant GetMeta(StringName name, Variant @default = default) =>
        _meta != null && _meta.TryGetValue(name.ToString(), out Variant v) ? v : @default;

    public bool HasMeta(StringName name) => _meta != null && _meta.ContainsKey(name.ToString());
    public void RemoveMeta(StringName name) => _meta?.Remove(name.ToString());
    public StringName[] GetMetaList() => _meta == null ? System.Array.Empty<StringName>() : _meta.Keys.Select(k => new StringName(k)).ToArray();

    // ---- signals ----

    private sealed class Connection
    {
        public Callable Callable;
        public uint Flags;
        public int RefCount = 1;
        public long Serial = ++s_connectionSerial;
    }

    private static long s_connectionSerial;

    /// <summary>The serial of the newest connection of any object: a later <see cref="EmitSignalThrough"/> reaches the connections made up to now.</summary>
    internal static long ConnectionSerial => s_connectionSerial;

    private Dictionary<string, List<Connection>>? _conns;
    private HashSet<string>? _userSignals;
    private bool _blockSignals;

    private static readonly Dictionary<System.Type, HashSet<string>> s_declaredSignals = new();

    // The signals a type declares: [Signal] delegates (name minus "EventHandler") and the names in its nested SignalName class.
    private static HashSet<string> DeclaredSignals(System.Type type)
    {
        lock (s_declaredSignals)
        {
            if (s_declaredSignals.TryGetValue(type, out var cached)) return cached;
            var set = new HashSet<string>();
            for (System.Type? t = type; t != null && t != typeof(object); t = t.BaseType)
            {
                foreach (System.Type nested in t.GetNestedTypes(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                {
                    if (typeof(Delegate).IsAssignableFrom(nested) && nested.GetCustomAttribute<SignalAttribute>() != null)
                    {
                        string n = nested.Name;
                        set.Add(n.EndsWith("EventHandler", StringComparison.Ordinal) ? n[..^"EventHandler".Length] : n);
                    }
                    else if (nested.Name == "SignalName")
                    {
                        foreach (FieldInfo f in nested.GetFields(BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly))
                            if (f.FieldType == typeof(StringName) && f.GetValue(null) is StringName sn) set.Add(sn.ToString());
                    }
                }
            }
            s_declaredSignals[type] = set;
            return set;
        }
    }

    // A scene or a converted script may spell a signal "hit", "Hit" or "body_entered" for the one declared as "Hit" / "body_entered":
    // GDScript names are snake_case, GodotSharp's are PascalCase. An exact name wins; otherwise the match ignores case and underscores.
    private string CanonicalSignal(string name)
    {
        HashSet<string> declared = DeclaredSignals(GetType());
        if (declared.Contains(name) || (_userSignals?.Contains(name) ?? false)) return name;
        static string Norm(string s) => s.Replace("_", "").ToLowerInvariant();
        string key = Norm(name);
        foreach (string d in declared) if (Norm(d) == key) return d;
        if (_userSignals != null) foreach (string d in _userSignals) if (Norm(d) == key) return d;
        return name;
    }

    public bool HasSignal(StringName signal)
    {
        string name = CanonicalSignal(signal.ToString());
        return DeclaredSignals(GetType()).Contains(name) || (_userSignals?.Contains(name) ?? false);
    }

    /// <summary>Declares a signal at runtime (Godot's add_user_signal).</summary>
    public void AddUserSignal(string signal, Collections.Array? arguments = null) => (_userSignals ??= new()).Add(signal);

    public bool IsBlockingSignals() => _blockSignals;
    public void SetBlockSignals(bool enable) => _blockSignals = enable;

    /// <summary>Connects a callable to a signal. The same callable twice fails with InvalidParameter unless ReferenceCounted.</summary>
    public Error Connect(StringName signal, Callable callable, uint flags = 0)
    {
        if (_freed) return Error.Unavailable;
        string name = CanonicalSignal(signal.ToString());
        if (!HasSignal(signal))
        {
            Log.Error($"[Fabric] {GetClass()} has no signal '{name}'");
            return Error.InvalidParameter;
        }
        if (callable.IsNull) return Error.InvalidParameter;
        var list = (_conns ??= new()).TryGetValue(name, out var l) ? l : _conns[name] = new List<Connection>();
        Connection? existing = list.Find(c => c.Callable == callable);
        if (existing != null)
        {
            if ((flags & (uint)ConnectFlags.ReferenceCounted) == 0)
            {
                Log.Error($"[Fabric] signal '{name}' of {GetClass()} is already connected to that callable");
                return Error.InvalidParameter;
            }
            existing.RefCount++;
            return Error.Ok;
        }
        list.Add(new Connection { Callable = callable, Flags = flags });
        return Error.Ok;
    }

    /// <summary>Removes a connection (one count of a ReferenceCounted one). Logs an error when it does not exist.</summary>
    public void Disconnect(StringName signal, Callable callable)
    {
        string name = CanonicalSignal(signal.ToString());
        if (_conns != null && _conns.TryGetValue(name, out var list))
        {
            int i = list.FindIndex(c => c.Callable == callable);
            if (i >= 0)
            {
                if (--list[i].RefCount <= 0) list.RemoveAt(i);
                return;
            }
        }
        Log.Error($"[Fabric] cannot disconnect '{name}' of {GetClass()}: no such connection");
    }

    public bool IsConnected(StringName signal, Callable callable) =>
        _conns != null && _conns.TryGetValue(CanonicalSignal(signal.ToString()), out var list) && list.Exists(c => c.Callable == callable);

    public bool HasConnections(StringName signal) => _conns != null && _conns.TryGetValue(CanonicalSignal(signal.ToString()), out var list) && list.Count > 0;

    /// <summary>The callables connected to a signal, in connection order.</summary>
    public Callable[] GetSignalConnections(StringName signal) =>
        _conns != null && _conns.TryGetValue(CanonicalSignal(signal.ToString()), out var list) ? list.Select(c => c.Callable).ToArray() : System.Array.Empty<Callable>();

    /// <summary>Emits a signal: every connected callable runs in connection order, a Deferred one when the tick pass ends.
    /// A handler that throws is logged and the rest still run. Connections to freed objects are dropped.</summary>
    public Error EmitSignal(StringName signal, params Variant[] args) => EmitSignalThrough(signal, long.MaxValue, args);

    /// <summary>Like <see cref="EmitSignal"/>, but only to the connections whose serial is at most <paramref name="serial"/>: the ones made before a mark.</summary>
    internal Error EmitSignalThrough(StringName signal, long serial, Variant[] args)
    {
        if (_freed) return Error.Unavailable;
        if (_blockSignals) return Error.Ok;
        string name = CanonicalSignal(signal.ToString());
        if (!HasSignal(signal))
        {
            Log.Error($"[Fabric] cannot emit '{name}': {GetClass()} has no such signal");
            return Error.Unavailable;
        }
        if (_conns == null || !_conns.TryGetValue(name, out var list) || list.Count == 0) return Error.Ok;
        args ??= System.Array.Empty<Variant>();
        foreach (Connection c in list.ToArray())
        {
            if (!list.Contains(c) || c.Serial > serial) continue;   // disconnected by an earlier handler, or made after the mark
            if (!c.Callable.IsValid()) { list.Remove(c); continue; }
            if ((c.Flags & (uint)ConnectFlags.OneShot) != 0) list.Remove(c);
            Callable cb = c.Callable;
            if ((c.Flags & (uint)ConnectFlags.Deferred) != 0)
                Deferred.Call(() => Invoke(cb, args, name));
            else
                Invoke(cb, args, name);
        }
        return Error.Ok;
    }

    private void Invoke(Callable cb, Variant[] args, string signal)
    {
        if (!cb.IsValid()) return;
        try { cb.Call(args); }
        catch (Exception ex) { Log.Error($"[Fabric] a handler of {GetClass()}.{signal} threw: {ex.GetType().Name}: {ex.Message}"); }
    }

    /// <summary>Awaitable: <c>await ToSignal(timer, Timer.SignalName.Timeout);</c></summary>
    public SignalAwaiter ToSignal(GodotObject source, StringName signal) => new(source, signal, this);

    /// <summary>Connects every signal's handler declared with the C# convention: for each [Signal] delegate "Hit", an event
    /// <c>Hit</c> added by the script adds to the same list. Kept for the converter's generated events.</summary>
    protected Error ConnectEvent(StringName signal, Delegate handler) => Connect(signal, Callable.From(handler));

    protected void DisconnectEvent(StringName signal, Delegate handler)
    {
        if (IsConnected(signal, Callable.From(handler))) Disconnect(signal, Callable.From(handler));
    }
}
