// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Fabric nodes that front a class registered by a native GDExtension plugin. A converted level places one as a class placement
// ("GDExtensionNode" or "GDExtensionNode3D") with the Prop extension_class = "TheClass"; ClassDB.Instantiate("TheClass") makes one
// too. The node owns a native object (modules/fabric.gdext): the extension's virtuals (_ready, _process, ...) run from this node's
// hooks in Godot's order, its methods and properties answer to Call/Get/Set, and its signals are signals of this node.

namespace Aver.Fabric;

/// <summary>A node whose behaviour comes from a native GDExtension class that extends Node.</summary>
public partial class GDExtensionNode : Node, IGdextHost
{
    private GdextPeer? _peer;

    /// <summary>The registered name of the native class (a converted level writes it as the Prop extension_class).</summary>
    [Export] public string ExtensionClass { get; set; } = "";

    GdextPeer? IGdextHost.Peer => _peer;

    /// <summary>The native object's handle, 0 before it exists.</summary>
    public int NativeHandle => _peer?.Handle ?? 0;

    public GDExtensionNode() { }

    internal static GDExtensionNode AdoptNative(int handle)
    {
        var n = new GDExtensionNode();
        n._peer = GdextPeer.Adopt(n, handle);
        n.ExtensionClass = n._peer.ClassName;
        return n;
    }

    /// <summary>Makes the native object if the class is known and it does not exist yet. False when there is none.</summary>
    public bool EnsureNative()
    {
        if (_peer == null && ExtensionClass.Length > 0) _peer = GdextPeer.Create(this, ExtensionClass);
        return _peer != null;
    }

    public override void _EnterTree() { if (EnsureNative()) _peer!.EnterTree(); }

    public override void _Ready()
    {
        if (_peer == null) return;
        (bool process, bool physics) = _peer.Ready();
        SetProcess(process);
        SetPhysicsProcess(physics);
    }

    public override void _ExitTree() => _peer?.ExitTree();
    public override void _Process(double delta) => _peer?.Process(delta, false);
    public override void _PhysicsProcess(double delta) => _peer?.Process(delta, true);

    public override void _Notification(int what)
    {
        base._Notification(what);
        _peer?.Notification(what);
    }

    public override void Free()
    {
        if (!IsAlive) return;
        base.Free();
        _peer?.Dispose();
        _peer = null;
    }

    internal override bool TryDynamicCall(string method, Variant[] args, out Variant result)
    {
        result = default;
        return EnsureNative() && _peer!.TryCall(method, args, out result);
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        return EnsureNative() && _peer!.TryGet(name, out value);
    }

    internal override bool TryDynamicSet(string name, Variant value) => EnsureNative() && _peer!.TrySet(name, value);
    internal override bool HasDynamicMethod(string name) => EnsureNative() && _peer!.HasMethod(name);
}

/// <summary>A 3D node whose behaviour comes from a native GDExtension class that extends Node3D. The node's transform and the native
/// object's stay in step: the extension sees what scripts set, and what it sets (set_position ...) reaches the node.</summary>
public partial class GDExtensionNode3D : Node3D, IGdextHost
{
    private GdextPeer? _peer;

    [Export] public string ExtensionClass { get; set; } = "";

    GdextPeer? IGdextHost.Peer => _peer;

    public int NativeHandle => _peer?.Handle ?? 0;

    public GDExtensionNode3D() { }

    internal static GDExtensionNode3D AdoptNative(int handle)
    {
        var n = new GDExtensionNode3D();
        n._peer = GdextPeer.Adopt(n, handle);
        n.ExtensionClass = n._peer.ClassName;
        return n;
    }

    public bool EnsureNative()
    {
        if (_peer == null && ExtensionClass.Length > 0) _peer = GdextPeer.Create(this, ExtensionClass);
        return _peer != null;
    }

    public override void _EnterTree() { if (EnsureNative()) _peer!.EnterTree(); }

    public override void _Ready()
    {
        if (_peer == null) return;
        (bool process, bool physics) = _peer.Ready();
        SetProcess(process);
        SetPhysicsProcess(physics);
    }

    public override void _ExitTree() => _peer?.ExitTree();
    public override void _Process(double delta) => _peer?.Process(delta, false);
    public override void _PhysicsProcess(double delta) => _peer?.Process(delta, true);

    public override void _Notification(int what)
    {
        base._Notification(what);
        _peer?.Notification(what);
    }

    public override void Free()
    {
        if (!IsAlive) return;
        base.Free();
        _peer?.Dispose();
        _peer = null;
    }

    internal override bool TryDynamicCall(string method, Variant[] args, out Variant result)
    {
        result = default;
        return EnsureNative() && _peer!.TryCall(method, args, out result);
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        return EnsureNative() && _peer!.TryGet(name, out value);
    }

    internal override bool TryDynamicSet(string name, Variant value) => EnsureNative() && _peer!.TrySet(name, value);
    internal override bool HasDynamicMethod(string name) => EnsureNative() && _peer!.HasMethod(name);
}

/// <summary>An object that is not a node (a native class extending Object, RefCounted or Resource), made by
/// <see cref="GDExtensionManager.Instantiate"/>.</summary>
public class GDExtensionObject : RefCounted, IGdextHost
{
    private GdextPeer? _peer;

    public string ExtensionClass { get; private set; } = "";

    GdextPeer? IGdextHost.Peer => _peer;

    public int NativeHandle => _peer?.Handle ?? 0;

    public GDExtensionObject() { }

    internal GDExtensionObject(string extensionClass)
    {
        ExtensionClass = extensionClass;
        _peer = GdextPeer.Create(this, extensionClass);
    }

    public override string GetClass() => ExtensionClass.Length > 0 ? ExtensionClass : base.GetClass();

    public override void Free()
    {
        base.Free();
        _peer?.Dispose();
        _peer = null;
    }

    internal override bool TryDynamicCall(string method, Variant[] args, out Variant result)
    {
        result = default;
        return _peer != null && _peer.TryCall(method, args, out result);
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        return _peer != null && _peer.TryGet(name, out value);
    }

    internal override bool TryDynamicSet(string name, Variant value) => _peer != null && _peer.TrySet(name, value);
    internal override bool HasDynamicMethod(string name) => _peer != null && _peer.HasMethod(name);
}
