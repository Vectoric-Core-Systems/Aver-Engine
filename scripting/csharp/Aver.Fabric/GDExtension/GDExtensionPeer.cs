// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The link between a Fabric node (or object) and the native object a GDExtension class made: calls, properties, signals, the
// virtuals in Godot's lifecycle order, the Node3D transform, and children the extension added.

using Aver.Scripting;

namespace Aver.Fabric;

/// <summary>Implemented by the Fabric types that front a native GDExtension object.</summary>
internal interface IGdextHost
{
    GdextPeer? Peer { get; }
}

internal sealed class GdextPeer
{
    private static readonly Dictionary<int, GdextPeer> s_byHandle = new();

    // One delegate for every peer, held here so the native side never calls a collected one.
    private static readonly GdextNative.SignalFn s_signalCallback = OnSignal;

    // The engine's own Node/Node3D signals are raised by the managed Node lifecycle; only the extension's signals are forwarded.
    private static readonly HashSet<string> s_engineSignals = new(StringComparer.Ordinal)
    {
        "ready", "renamed", "tree_entered", "tree_exiting", "tree_exited", "child_entered_tree", "child_exiting_tree",
        "child_order_changed", "visibility_changed", "script_changed", "property_list_changed", "changed",
    };

    internal int Handle { get; }
    internal GodotObject Owner { get; }
    internal string ClassName { get; }

    private int _xformVersion;
    private Transform3D _pushed;
    private bool _hasPushed;
    private bool _disposed;

    private GdextPeer(int handle, GodotObject owner, string className)
    {
        Handle = handle;
        Owner = owner;
        ClassName = className;
    }

    internal long InstanceId => GdextNative.aver_gdext_object_instance_id(Handle);

    internal static GdextPeer? Of(GodotObject o) => o is IGdextHost h ? h.Peer : null;

    internal static GdextPeer? ByHandle(int handle) => s_byHandle.TryGetValue(handle, out GdextPeer? p) ? p : null;

    /// <summary>Makes a native object of an extension class (or a host class) and links it to <paramref name="owner"/>.</summary>
    internal static GdextPeer? Create(GodotObject owner, string className)
    {
        if (!GDExtensionManager.IsNativeAvailable) return null;
        if (!GDExtensionManager.ClassExists(className)) GDExtensionManager.EnsureProjectExtensions();   // a converted level's plugins
        int handle = GdextNative.aver_gdext_instantiate(className);
        if (handle == 0)
        {
            Log.Error($"[Fabric] GDExtension: cannot create '{className}': {GdextNative.LastError()}");
            return null;
        }
        return Link(owner, handle, className);
    }

    /// <summary>Links <paramref name="owner"/> to a native object that already exists (a child an extension created).</summary>
    internal static GdextPeer Adopt(GodotObject owner, int handle) => Link(owner, handle, GdextNative.ObjectClass(handle));

    private static GdextPeer Link(GodotObject owner, int handle, string className)
    {
        var peer = new GdextPeer(handle, owner, className);
        s_byHandle[handle] = peer;
        // the extension's signals become signals of the managed object, so Connect/EmitSignal/ToSignal work on them
        for (string c = className; c.Length > 0; c = GdextNative.ClassParent(c))
            for (int i = 0, n = GdextNative.aver_gdext_signal_count(c); i < n; i++)
                owner.AddUserSignal(GdextNative.SignalName(c, i));
        GdextNative.aver_gdext_listen(handle, s_signalCallback, IntPtr.Zero);
        return peer;
    }

    internal void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        s_byHandle.Remove(Handle);
        if (GdextNative.aver_gdext_object_is_valid(Handle) != 0) GdextNative.aver_gdext_free(Handle);
    }

    private static void OnSignal(IntPtr user, int obj, IntPtr signal, IntPtr args, int len)
    {
        try
        {
            if (!s_byHandle.TryGetValue(obj, out GdextPeer? peer)) return;
            string name = GdextNative.Str(signal);
            if (s_engineSignals.Contains(name)) return;
            var bytes = new byte[len];
            if (len > 0) System.Runtime.InteropServices.Marshal.Copy(args, bytes, 0, len);
            peer.Owner.EmitSignal(name, VariantWire.DecodeArguments(bytes));
        }
        catch (Exception ex)
        {
            Log.Error($"[Fabric] GDExtension signal handler failed: {ex.GetType().Name}: {ex.Message}");
        }
    }

    // ---- members by name ----

    internal bool HasMethod(string method) => !_disposed && GdextNative.aver_gdext_has_method(Handle, method) != 0;

    internal bool TryCall(string method, Variant[] args, out Variant result)
    {
        result = default;
        if (!HasMethod(method)) return false;
        PushTransform();
        byte[] a = VariantWire.EncodeArguments(args);
        int n = GdextNative.aver_gdext_call(Handle, method, a, a.Length);
        if (n < 0) throw new InvalidOperationException($"GDExtension call {ClassName}.{method} failed: {GdextNative.LastError()}");
        result = VariantWire.Decode(GdextNative.Result(n));
        Pump();
        return true;
    }

    internal bool TryGet(string property, out Variant value)
    {
        value = default;
        if (_disposed) return false;
        PushTransform();
        int n = GdextNative.aver_gdext_get(Handle, property);
        if (n < 0) return false;
        value = VariantWire.Decode(GdextNative.Result(n));
        return true;
    }

    internal bool TrySet(string property, Variant value)
    {
        if (_disposed) return false;
        byte[] v = VariantWire.Encode(value);
        bool ok = GdextNative.aver_gdext_set(Handle, property, v, v.Length) == 1;
        if (ok) Pump();
        return ok;
    }

    // ---- lifecycle ----

    internal void EnterTree()
    {
        if (_disposed) return;
        if (Owner is Node node)
        {
            GdextNative.aver_gdext_node_set_name(Handle, node.Name.ToString());
            Node? parent = node.GetParent();
            GdextNative.aver_gdext_node_set_parent(Handle, parent is IGdextHost { Peer: { } pp } ? pp.Handle : 0);
        }
        PushTransform();
        GdextNative.aver_gdext_enter_tree(Handle, 0);
        Pump();
    }

    /// <summary>Runs the extension's _ready and tells the node whether the extension wants _process and _physics_process.</summary>
    internal (bool Process, bool Physics) Ready()
    {
        if (_disposed) return (false, false);
        PushTransform();
        GdextNative.aver_gdext_ready(Handle, 0);
        Pump();
        int f = GdextNative.aver_gdext_node_process_flags(Handle);
        return ((f & 1) != 0, (f & 2) != 0);
    }

    internal void ExitTree()
    {
        if (_disposed) return;
        GdextNative.aver_gdext_exit_tree(Handle, 0);
        Pump();
    }

    internal void Process(double delta, bool physics)
    {
        if (_disposed) return;
        PushTransform();
        GdextNative.aver_gdext_process(Handle, delta, physics ? 1 : 0, 0);
        Pump();
        GdextNative.aver_gdext_flush_deferred();
    }

    // The managed node sends these itself (ENTER_TREE, EXIT_TREE, READY and the process ones), and the native calls above send them
    // to the extension; forwarding them again would count each twice.
    internal void Notification(int what)
    {
        if (_disposed || what is Node.NotificationEnterTree or Node.NotificationExitTree or Node.NotificationReady
                or Node.NotificationProcess or Node.NotificationPhysicsProcess or Node.NotificationPostEnterTree) return;
        GdextNative.aver_gdext_notification(Handle, what);
    }

    // ---- Node3D transform and children the extension added ----

    private static float[] ToFloats(Transform3D t) => new[]
    {
        t.Basis.Row0.X, t.Basis.Row0.Y, t.Basis.Row0.Z, t.Basis.Row1.X, t.Basis.Row1.Y, t.Basis.Row1.Z,
        t.Basis.Row2.X, t.Basis.Row2.Y, t.Basis.Row2.Z, t.Origin.X, t.Origin.Y, t.Origin.Z,
    };

    private static Transform3D FromFloats(float[] f) =>
        new(new Basis { Row0 = new Vector3(f[0], f[1], f[2]), Row1 = new Vector3(f[3], f[4], f[5]), Row2 = new Vector3(f[6], f[7], f[8]) },
            new Vector3(f[9], f[10], f[11]));

    internal void PushTransform()
    {
        if (Owner is not Node3D n3 || _disposed) return;
        Transform3D t = n3.Transform;
        if (_hasPushed && t == _pushed) return;
        GdextNative.aver_gdext_node3d_set_transform(Handle, ToFloats(t));
        _pushed = t;
        _hasPushed = true;
        _xformVersion = GdextNative.aver_gdext_node3d_transform_version(Handle);
    }

    /// <summary>Takes what the extension changed: its transform, and the children it added.</summary>
    internal void Pump()
    {
        if (_disposed) return;
        if (Owner is Node3D n3)
        {
            int version = GdextNative.aver_gdext_node3d_transform_version(Handle);
            if (version != _xformVersion)
            {
                _xformVersion = version;
                var f = new float[12];
                if (GdextNative.aver_gdext_node3d_get_transform(Handle, f) == 1)
                {
                    n3.Transform = FromFloats(f);
                    _pushed = n3.Transform;
                    _hasPushed = true;
                }
            }
        }
        if (Owner is not Node self) return;
        int child;
        while ((child = GdextNative.aver_gdext_node_pending_child(Handle)) != 0)
        {
            Node? wrapped = WrapChild(child);
            if (wrapped != null && wrapped.GetParent() == null) self.AddChild(wrapped);
        }
    }

    private static Node? WrapChild(int handle)
    {
        if (s_byHandle.TryGetValue(handle, out GdextPeer? known)) return known.Owner as Node;
        string cls = GdextNative.ObjectClass(handle);
        Node node = GdextNative.aver_gdext_class_is_a(cls, "Node3D") != 0 ? GDExtensionNode3D.AdoptNative(handle) : GDExtensionNode.AdoptNative(handle);
        node.Name = GdextNative.NodeName(handle);
        return node;
    }
}
