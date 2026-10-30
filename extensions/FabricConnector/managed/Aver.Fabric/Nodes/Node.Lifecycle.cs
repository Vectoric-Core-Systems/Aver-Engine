// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Node lifecycle: Godot's virtuals (_EnterTree, _Ready, _Process ...) mapped onto the framework hooks, the exported values a
// level carries, and the processing switches.

using Aver.Framework;
using Aver.Scripting;

namespace Aver.Fabric;

public partial class Node
{
    // ---- state ----

    private bool _insideTree;
    private bool _readyDone;            // _Ready has run (it runs once, on the first time in the tree)
    private bool _propsApplied;
    private bool _process, _physicsProcess, _processInput, _processUnhandledInput, _processUnhandledKeyInput;

    /// <summary>Exported values (property key and Godot-text value) to apply as the node enters the tree, before _EnterTree.
    /// A level fills this from COMPONENT Props; the managed host and tests set it directly.</summary>
    internal List<(string Key, string Text)>? PendingValues;

    // ---- Godot's virtuals ----

    /// <summary>Called when the node enters the tree, parents before children.</summary>
    public virtual void _EnterTree() { }

    /// <summary>Called when the node leaves the tree, children before parents.</summary>
    public virtual void _ExitTree() { }

    /// <summary>Called once when the node and all its children have entered the tree: children first.</summary>
    public virtual void _Ready() { }

    /// <summary>Called every frame. <paramref name="delta"/> is the frame time in seconds, scaled by the time scale.</summary>
    public virtual void _Process(double delta) { }

    /// <summary>Called on every fixed physics step.</summary>
    public virtual void _PhysicsProcess(double delta) { }

    /// <summary>Called for each input event, before the UI and before <see cref="_UnhandledInput"/>. Deepest, last-added nodes first.</summary>
    public virtual void _Input(InputEvent @event) { }

    /// <summary>Called for input events nothing before it handled (call <c>GetViewport().SetInputAsHandled()</c> to stop an event).</summary>
    public virtual void _UnhandledInput(InputEvent @event) { }

    public virtual void _UnhandledKeyInput(InputEvent @event) { }

    /// <summary>Called for engine-driven notifications of a node type, before the scripts' <c>_Process</c>. Fabric classes override
    /// this for their own per-frame work (a Timer counting down) without touching the script's switches.</summary>
    internal virtual void InternalProcess(double delta) { }
    internal virtual void InternalPhysicsProcess(double delta) { }
    internal virtual bool UsesInternalProcess => false;
    internal virtual bool UsesInternalPhysicsProcess => false;

    // ---- framework hooks to Godot's order ----

    /// <inheritdoc/>
    public override void OnEnterTree() => NotifyEnterTree();

    /// <inheritdoc/>
    public override void OnBeginPlay(BeginReason reason)
    {
        if (reason == BeginReason.Reload) return;
        NotifyReady();
    }

    /// <inheritdoc/>
    public override void OnExitTree() => NotifyExitTree();

    /// <inheritdoc/>
    public override void OnEndPlay(EndReason reason)
    {
        if (reason == EndReason.Reload) return;
        // The entity is gone: the node leaves its parent's list quietly and is freed.
        if (_insideTree) NotifyExitTree();
        if (_parent != null) { _parent._children.Remove(this); _parent = null; }
        FreeSubtreeMarks();
    }

    /// <inheritdoc/>
    public override void OnParentChanged() => _Notification(IsParented() ? NotificationParented : NotificationUnparented);

    private bool IsParented() => Self.IsValid ? Self.Parent.IsValid : _parent != null;

    /// <inheritdoc/>
    public override void OnTick(float dt)
    {
        if (!_insideTree) return;
        if (UsesInternalProcess) InternalProcess(dt);
        if (_process) _Process(dt);
    }

    /// <summary>The fixed physics step of the pass that is running, in seconds.</summary>
    internal static float PhysicsDelta { get; private set; } = 1f / 60f;

    /// <inheritdoc/>
    public override void OnPhysicsTick(float dt)
    {
        PhysicsDelta = dt;
        if (!_insideTree) return;
        if (UsesInternalPhysicsProcess) InternalPhysicsProcess(dt);
        if (_physicsProcess) _PhysicsProcess(dt);
    }

    /// <inheritdoc/>
    public override void OnInput(Aver.Framework.InputEvent e)
    {
        if (_insideTree && _processInput) DeliverFramework(e, unhandled: false);
    }

    /// <inheritdoc/>
    public override void OnUnhandledInput(Aver.Framework.InputEvent e)
    {
        if (_insideTree && (_processUnhandledInput || _processUnhandledKeyInput)) DeliverFramework(e, unhandled: true);
    }

    // An engine event reaches the script as the Godot event it bridges to; Current lets GetViewport().SetInputAsHandled() find it.
    private void DeliverFramework(Aver.Framework.InputEvent e, bool unhandled)
    {
        InputEvent ge = InputBridge.FromFramework(e);
        InputEvent? outer = InputEvent.Current;
        InputEvent.Current = ge;
        try { DeliverInput(ge, unhandled); }
        finally { InputEvent.Current = outer; }
    }

    /// <summary>Calls the node's _Input (or, for the unhandled pass, _UnhandledInput and _UnhandledKeyInput) when it is processing them.</summary>
    internal void DeliverInput(InputEvent e, bool unhandled)
    {
        if (!unhandled)
        {
            if (_processInput) _Input(e);
            return;
        }
        if (_processUnhandledInput) _UnhandledInput(e);
        if (_processUnhandledKeyInput && e is InputEventKey && !e.IsHandled) _UnhandledKeyInput(e);
    }

    // ---- the notifications, run by the hooks above or by the managed host ----

    /// <summary>Enter the tree: link to the parent, read the level's values, then _EnterTree and the signals.</summary>
    internal void NotifyEnterTree()
    {
        if (_insideTree) return;
        _insideTree = true;
        if (Self.IsValid && NodeHost.Current.IsEngine)
        {
            LinkToEntityParent();
            AbsorbEngineGroups();
            ApplyLevelValues();
            if (_persistent) base.Persistent = true;   // set by script before the node had an entity
        }
        else if (_parent == null) AttachToRoot();
        if (PendingValues != null)
        {
            var pending = PendingValues;
            PendingValues = null;
            ApplyValues(pending);
        }
        RefreshProcessFlags();
        OnEnteringTree();
        _Notification(NotificationEnterTree);
        _EnterTree();
        EmitSignal(SignalName.TreeEntered);
        _parent?.EmitSignal(SignalName.ChildEnteredTree, this);
        SceneTree.Instance.EmitNodeAdded(this);
    }

    /// <summary>Ready: once, after every child is ready.</summary>
    internal void NotifyReady()
    {
        if (!_insideTree) return;
        ResolvePendingNodeRefs();
        ApplyPendingConnections();
        _Notification(NotificationPostEnterTree);
        if (_readyDone) return;
        _readyDone = true;
        OnReadying();
        _Notification(NotificationReady);
        _Ready();
        EmitSignal(SignalName.Ready);
    }

    /// <summary>Exit the tree: the signal, _ExitTree, then tree_exited. Children have exited before this is called.</summary>
    internal void NotifyExitTree()
    {
        if (!_insideTree) return;
        EmitSignal(SignalName.TreeExiting);
        _Notification(NotificationExitTree);
        _ExitTree();
        OnExitingTree();
        _insideTree = false;
        SceneTree.Instance.EmitNodeRemoved(this);
        EmitSignal(SignalName.TreeExited);
    }

    // A node type's own work as it enters, becomes ready and leaves (a mesh node adds its mesh component, a Timer starts).
    internal virtual void OnEnteringTree() { }
    internal virtual void OnReadying() { }
    internal virtual void OnExitingTree() { }

    // The tree's own window: in the tree from the start, ready, with no entity.
    internal void MarkAsRoot()
    {
        _insideTree = true;
        _readyDone = true;
        _propsApplied = true;
    }

    // A scene root enters with the tree's window as its parent.
    private void AttachToRoot()
    {
        Node root = SceneTree.Instance.Root;
        if (this == root || _parent != null) return;
        _parent = root;
        if (!root._children.Contains(this)) root._children.Add(this);
        _name = root.UniqueChildName(this, _name, false);
    }

    // ---- process switches ----

    internal void RefreshProcessFlags()
    {
        TickEnabled = _process || UsesInternalProcess;
        PhysicsTickEnabled = _physicsProcess || UsesInternalPhysicsProcess;
        InputEnabled = _processInput || _processUnhandledInput || _processUnhandledKeyInput;
    }

    public void SetProcess(bool enable) { _process = enable; RefreshProcessFlags(); }
    public bool IsProcessing() => _process;
    public void SetPhysicsProcess(bool enable) { _physicsProcess = enable; RefreshProcessFlags(); }
    public bool IsPhysicsProcessing() => _physicsProcess;
    public void SetProcessInput(bool enable) { _processInput = enable; RefreshProcessFlags(); }
    public bool IsProcessingInput() => _processInput;
    public void SetProcessUnhandledInput(bool enable) { _processUnhandledInput = enable; RefreshProcessFlags(); }
    public bool IsProcessingUnhandledInput() => _processUnhandledInput;
    public void SetProcessUnhandledKeyInput(bool enable) { _processUnhandledKeyInput = enable; RefreshProcessFlags(); }
    public bool IsProcessingUnhandledKeyInput() => _processUnhandledKeyInput;

    /// <summary>Godot's process_mode: whether the node runs while the tree is paused (Inherit takes the parent's).</summary>
    public ProcessModeEnum ProcessMode
    {
        get => (ProcessModeEnum)(int)PauseMode;
        set => PauseMode = (Aver.Framework.PauseMode)(int)value;
    }

    /// <summary>Godot's process_priority: lower runs earlier in the _Process pass; equal priorities run in tree order.</summary>
    public int ProcessPriority { get => TickPriority; set => TickPriority = value; }

    /// <summary>Godot's process_physics_priority, for the _PhysicsProcess pass.</summary>
    public int ProcessPhysicsPriority { get => PhysicsTickPriority; set => PhysicsTickPriority = value; }

    /// <summary>Whether this node is running right now: in the tree and allowed by its process mode and the tree's pause.</summary>
    public bool CanProcess()
    {
        if (!_insideTree) return false;
        Aver.Framework.PauseMode m = Aver.Framework.PauseMode.Inherit;
        for (Node? n = this; n != null && m == Aver.Framework.PauseMode.Inherit; n = n._parent) m = n.PauseMode;
        bool paused = SceneTree.Instance.Paused;
        return m switch
        {
            Aver.Framework.PauseMode.Always => true,
            Aver.Framework.PauseMode.WhenPaused => paused,
            Aver.Framework.PauseMode.Disabled => false,
            _ => !paused,
        };
    }

    // ---- what the level carries ----

    // Reserved Props keys, written by the converter (docs/plans/fabric/G3_RUNTIME.md):
    //   _name    the node's name            _owner   a node path from this node to its owner (e.g. "..")
    //   _unique  1 = a unique name in its owner (%Name)   _connections  the scene's signal connections (see below)
    //   _persistent  1 = survives Game.ChangeLevel (an autoload)   _class  the Godot class of a plain placement (FromEntity)
    // Every other Props key is a Godot property name whose value is Godot's text form (VariantText).

    private void AbsorbEngineGroups()
    {
        foreach (string g in Aver.Framework.Groups.Of(Self))
        {
            _groups ??= new();
            if (!_groups.Add(g)) continue;
            lock (s_groupMembers)
            {
                if (!s_groupMembers.TryGetValue(g, out var list)) s_groupMembers[g] = list = new List<Node>();
                list.Add(this);
            }
        }
    }

    internal void ApplyLevelValues()
    {
        if (_propsApplied) return;
        _propsApplied = true;
        var values = Aver.Framework.Hosting.ExtensionHost.ReadProperties(Self);
        ApplyValues(values);
        // The bridge writes [Editable] fields before OnBeginPlay; these values are already in, and the keys are not [Editable] names.
        Aver.Framework.Hosting.ExtensionHost.MarkEditablesApplied(this);
    }

    // The converter writes every Props value as Godot's text, so a string is quoted ("Crate"); a hand-written level may leave it bare.
    internal static string PlainText(string text)
    {
        if (text.Length >= 2 && text[0] == '"' && text[^1] == '"' && VariantText.TryParse(text, out Variant v) && v.VariantType == Variant.Type.String)
            return v.AsString();
        return text;
    }

    /// <summary>Applies exported values (key and Godot-text value pairs) the way a scene instantiation does, before _EnterTree.</summary>
    internal void ApplyValues(IEnumerable<(string Key, string Text)> values)
    {
        string? ownerPath = null;
        bool unique = false;
        foreach ((string key, string text) in values)
        {
            switch (key)
            {
                case "_name": SetNameInternal(PlainText(text)); continue;
                case "_owner": ownerPath = PlainText(text); continue;
                case "_unique": unique = text is "1" or "true"; continue;
                case "_connections": _pendingConnections = text; continue;
                case "_persistent": Persistent = text is "1" or "true"; continue;
                case "_class":   // read by FromEntity for a plain placement; a class placement already is its class
                    if (this is Control && PlainText(text) is { Length: > 0 } carried && carried != GetClass()) SetMeta("_class", carried);   // a Button / Label carrier: theme lookups use the Godot class
                    continue;
            }
            if (!VariantText.TryParse(text, out Variant v))
            {
                Log.Warn($"[Fabric] {GetClass()} '{_name}': cannot read the value of '{key}': {text}");
                continue;
            }
            try
            {
                // "metadata/tag" is Godot's spelling of set_meta("tag", ...).
                if (key.StartsWith("metadata/", StringComparison.Ordinal))
                {
                    SetMeta(key["metadata/".Length..], v);
                    continue;
                }
                // A node-typed export is stored as a NodePath relative to this node; it is looked up now or, if its target has not
                // entered the tree yet, when this node becomes ready.
                if (v.VariantType is Variant.Type.NodePath or Variant.Type.String &&
                    MemberAccess.SettableType(GetType(), key) is { } mt && typeof(Node).IsAssignableFrom(mt))
                {
                    string path = v.AsString();
                    if (path.Length == 0) { MemberAccess.TrySet(this, key, default); continue; }
                    if (GetNodeOrNull(path) is Node found) MemberAccess.TrySet(this, key, found);
                    else (_pendingNodeRefs ??= new()).Add((key, path));
                    continue;
                }
                // A resource marked local_to_scene is not shared between instances: each node gets its own copy.
                if (v.VariantType == Variant.Type.Object && v.AsGodotObject() is Resource { ResourceLocalToScene: true } shared)
                {
                    Resource own = shared.Duplicate();
                    own._SetupLocalToScene();
                    v = own;
                }
                if (!MemberAccess.TrySet(this, key, v) && !TryDynamicSet(key, v))
                    Log.Warn($"[Fabric] {GetClass()} '{_name}' has no property '{key}'; the value is kept in metadata");
                    // kept so a script can still read it with GetMeta
                else continue;
                SetMeta(key, v);
            }
            catch (Exception ex)
            {
                Log.Warn($"[Fabric] {GetClass()} '{_name}': setting '{key}' failed: {ex.GetType().Name}: {ex.Message}");
            }
        }
        if (ownerPath != null && GetNodeOrNull(ownerPath) is Node owner) Owner = owner;
        if (unique) UniqueNameInOwner = true;
    }

    // The scene's [connection] lines for this node's signals (Props key _connections): an array of dictionaries with signal, to (a
    // path from this node), method, flags, binds, unbinds. Made when the node becomes ready, when every node of its batch has
    // entered the tree and so can be found by path, and before any _Ready of the batch that does not belong to a descendant.
    private string? _pendingConnections;
    private List<(string Key, string Path)>? _pendingNodeRefs;

    private void ResolvePendingNodeRefs()
    {
        if (_pendingNodeRefs == null) return;
        var refs = _pendingNodeRefs;
        _pendingNodeRefs = null;
        foreach ((string key, string path) in refs)
        {
            if (GetNodeOrNull(path) is Node found) MemberAccess.TrySet(this, key, found);
            else Log.Warn($"[Fabric] {GetClass()} '{_name}': '{key}' points at '{path}', which is not in the scene");
        }
    }

    private void ApplyPendingConnections()
    {
        if (_pendingConnections == null) return;
        string text = _pendingConnections;
        _pendingConnections = null;
        if (!VariantText.TryParse(text, out Variant parsed))
        {
            Log.Warn($"[Fabric] {GetClass()} '{_name}': cannot read its connections: {text}");
            return;
        }
        foreach (Variant item in parsed.AsGodotArray())
        {
            var d = item.AsGodotDictionary();
            string signal = d.Get("signal").AsString(), to = d.Get("to").AsString(), method = d.Get("method").AsString();
            Node? target = GetNodeOrNull(to);
            if (target == null)
            {
                Log.Warn($"[Fabric] {GetClass()} '{_name}': signal '{signal}' is connected to '{to}', which is not in the scene");
                continue;
            }
            if (!target.HasMethod(method))
            {
                // Godot would connect and fail at emit; a missing method here is the converted script's gap, so say so once, now.
                Log.Warn($"[Fabric] {GetClass()} '{_name}': signal '{signal}' is connected to {target.GetClass()}.{method}, which does not exist; the connection is skipped");
                continue;
            }
            var callable = new Callable(target, method);
            Collections.Array binds = d.Get("binds").AsGodotArray();
            if (binds.Count > 0) callable = callable.Bind(binds.ToArray());
            int unbinds = d.Get("unbinds").AsInt32();
            if (unbinds > 0) callable = callable.Unbind(unbinds);
            Connect(signal, callable, d.Get("flags").AsUInt32());
        }
    }

    // ---- a node around an existing entity ----

    private static readonly Dictionary<int, WeakReference<Node>> s_wrappers = new();

    /// <summary>The node for an entity: the actor bound to it when that is a Node, otherwise a Node3D made around it (kept, so asking
    /// twice gives the same object). Null for an invalid entity.</summary>
    public static Node? FromEntity(Entity entity)
    {
        if (!entity.IsValid) return null;
        if (Actors.Get(entity) is Node bound) return bound;
        lock (s_wrappers)
        {
            if (s_wrappers.TryGetValue(entity.Handle, out var wr) && wr.TryGetTarget(out Node? hit) && hit.IsAlive && hit.Self.Equals(entity)) return hit;
            Node3D w = NewWrapper(entity);
            Aver.Framework.Hosting.ExtensionHost.SetSelf(w, entity);
            w._name = entity.Name;
            w._insideTree = true;
            w._readyDone = true;
            s_wrappers[entity.Handle] = new WeakReference<Node>(w);
            return w;
        }
    }

    // A plain placement's Props may carry "_class" (the Godot class name) and "_name"; the wrapper takes that class when it is a
    // Node3D type without behaviour of its own to run (Marker3D, a mesh or light stand-in), otherwise a Node3D.
    private static Node3D NewWrapper(Entity entity)
    {
        var values = new List<(string Key, string Text)>();
        try { values = Aver.Framework.Hosting.ExtensionHost.ReadProperties(entity); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        return WrapperFor(values);
    }

    internal static Node3D WrapperFor(IEnumerable<(string Key, string Text)> props)
    {
        foreach ((string key, string text) in props)
        {
            if (key != "_class") continue;
            System.Type? t = ClassDB.TypeOf(PlainText(text));
            if (t != null && typeof(Node3D).IsAssignableFrom(t) && !t.IsAbstract && t.GetConstructor(System.Type.EmptyTypes) != null)
                return (Node3D)Activator.CreateInstance(t)!;
        }
        return new Node3D();
    }

    /// <summary>The Node bound to <paramref name="entity"/> as <typeparamref name="T"/>, or null.</summary>
    public static T? FromEntity<T>(Entity entity) where T : Node => FromEntity(entity) as T;
}
