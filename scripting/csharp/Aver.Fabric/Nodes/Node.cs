// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Node: Godot's scene-tree node on an Aver actor. Identity, the child list, names, paths, groups and deferred destruction.
// The lifecycle (enter, ready, exit, process, input) is Node.Lifecycle.cs; how a node gets an entity is NodeHost.cs.

using Aver.Framework;
using Aver.Scripting;

namespace Aver.Fabric;

/// <summary>A node of the scene tree (Godot's Node). See docs/plans/fabric/G3_RUNTIME.md for how a node maps to an entity.</summary>
public partial class Node : GodotObject
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName Ready = "ready";
        public static readonly StringName Renamed = "renamed";
        public static readonly StringName TreeEntered = "tree_entered";
        public static readonly StringName TreeExiting = "tree_exiting";
        public static readonly StringName TreeExited = "tree_exited";
        public static readonly StringName ChildEnteredTree = "child_entered_tree";
        public static readonly StringName ChildExitingTree = "child_exiting_tree";
        public static readonly StringName ChildOrderChanged = "child_order_changed";
        public static readonly StringName ReplacingBy = "replacing_by";
    }

    public new class MethodName : GodotObject.MethodName
    {
        public static readonly StringName _EnterTree = "_EnterTree";
        public static readonly StringName _ExitTree = "_ExitTree";
        public static readonly StringName _Ready = "_Ready";
        public static readonly StringName _Process = "_Process";
        public static readonly StringName _PhysicsProcess = "_PhysicsProcess";
        public static readonly StringName _Input = "_Input";
        public static readonly StringName _UnhandledInput = "_UnhandledInput";
        public static readonly StringName QueueFree = "QueueFree";
        public static readonly StringName AddChild = "AddChild";
        public static readonly StringName RemoveChild = "RemoveChild";
        public static readonly StringName GetNode = "GetNode";
        public static readonly StringName GetParent = "GetParent";
    }

    public new class PropertyName : GodotObject.PropertyName
    {
        public static readonly StringName Name = "name";
        public static readonly StringName Owner = "owner";
        public static readonly StringName ProcessMode = "process_mode";
        public static readonly StringName ProcessPriority = "process_priority";
        public static readonly StringName ProcessPhysicsPriority = "process_physics_priority";
        public static readonly StringName UniqueNameInOwner = "unique_name_in_owner";
        public static readonly StringName SceneFilePath = "scene_file_path";
    }

    public event Action Ready { add => Connect(SignalName.Ready, Callable.From(value)); remove => Disconnect(SignalName.Ready, Callable.From(value)); }
    public event Action Renamed { add => Connect(SignalName.Renamed, Callable.From(value)); remove => Disconnect(SignalName.Renamed, Callable.From(value)); }
    public event Action TreeEntered { add => Connect(SignalName.TreeEntered, Callable.From(value)); remove => Disconnect(SignalName.TreeEntered, Callable.From(value)); }
    public event Action TreeExiting { add => Connect(SignalName.TreeExiting, Callable.From(value)); remove => Disconnect(SignalName.TreeExiting, Callable.From(value)); }
    public event Action TreeExited { add => Connect(SignalName.TreeExited, Callable.From(value)); remove => Disconnect(SignalName.TreeExited, Callable.From(value)); }
    public event Action<Node> ChildEnteredTree { add => Connect(SignalName.ChildEnteredTree, Callable.From(value)); remove => Disconnect(SignalName.ChildEnteredTree, Callable.From(value)); }
    public event Action<Node> ChildExitingTree { add => Connect(SignalName.ChildExitingTree, Callable.From(value)); remove => Disconnect(SignalName.ChildExitingTree, Callable.From(value)); }

    /// <summary>Godot's process_mode.</summary>
    public enum ProcessModeEnum { Inherit = 0, Pausable = 1, WhenPaused = 2, Always = 3, Disabled = 4 }

    public enum InternalMode { Disabled = 0, Front = 1, Back = 2 }

    [Flags]
    public enum DuplicateFlags { Signals = 1, Groups = 2, Scripts = 4, UseInstantiation = 8 }

    public const int NotificationEnterTree = 10;
    public const int NotificationExitTree = 11;
    public const int NotificationReady = 13;
    public const int NotificationPaused = 14;
    public const int NotificationUnpaused = 15;
    public const int NotificationPhysicsProcess = 16;
    public const int NotificationProcess = 17;
    public const int NotificationParented = 18;
    public const int NotificationUnparented = 19;
    public const int NotificationSceneInstantiated = 20;
    public const int NotificationPathRenamed = 23;
    public const int NotificationChildOrderChanged = 24;
    public const int NotificationInternalProcess = 25;
    public const int NotificationInternalPhysicsProcess = 26;
    public const int NotificationPostEnterTree = 27;
    public const int NotificationDisabled = 28;
    public const int NotificationEnabled = 29;

    // ---- construction ----

    private static readonly Dictionary<System.Type, Overrides> s_overrides = new();

    // Which virtuals the concrete class overrides, so a class that does not use _Process does not tick.
    private sealed record Overrides(bool Process, bool Physics, bool Input, bool Unhandled, bool UnhandledKey, bool Notification);

    private readonly Overrides _virtuals;

    public Node()
    {
        System.Type t = GetType();
        lock (s_overrides)
        {
            if (!s_overrides.TryGetValue(t, out Overrides? o))
            {
                static bool Has(System.Type type, string name, params System.Type[] args)
                {
                    System.Reflection.MethodInfo? m = type.GetMethod(name, System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic, null, args, null);
                    return m != null && m.DeclaringType != typeof(Node) && m.GetBaseDefinition().DeclaringType == typeof(Node);
                }
                o = new Overrides(Has(t, nameof(_Process), typeof(double)), Has(t, nameof(_PhysicsProcess), typeof(double)),
                                  Has(t, nameof(_Input), typeof(InputEvent)), Has(t, nameof(_UnhandledInput), typeof(InputEvent)),
                                  Has(t, nameof(_UnhandledKeyInput), typeof(InputEvent)), Has(t, nameof(_Notification), typeof(int)));
                s_overrides[t] = o;
            }
            _virtuals = o;
        }
        _process = _virtuals.Process;
        _physicsProcess = _virtuals.Physics;
        _processInput = _virtuals.Input;
        _processUnhandledInput = _virtuals.Unhandled;
        _processUnhandledKeyInput = _virtuals.UnhandledKey;
        RefreshProcessFlags();
    }

    // ---- name, parent, children ----

    private string _name = "";
    private Node? _parent;
    private readonly List<Node> _children = new();
    private Node? _owner;
    private bool _uniqueNameInOwner;
    private Dictionary<string, Node>? _uniqueOwned;
    private string _sceneFilePath = "";

    /// <summary>The node's name, unique among its siblings. A name that clashes is changed (see AddChild).</summary>
    public StringName Name
    {
        get => _name;
        set => SetNameInternal(value?.ToString() ?? "");
    }

    internal void SetNameInternal(string value)
    {
        value = value.Replace(".", "").Replace(":", "").Replace("/", "").Replace("@", "").Replace("%", "");
        if (value == _name) return;
        string old = _name;
        _name = value;
        if (_parent != null) _name = _parent.UniqueChildName(this, value, false);
        if (Self.IsValid) Self.SetName(_name);
        if (old.Length > 0 || _parent != null)
        {
            EmitSignal(SignalName.Renamed);
            if (_insideTree && old.Length > 0) SceneTree.Instance.EmitNodeRenamed(this);
        }
    }

    /// <summary>The parent in the tree, or null for a node not in one. A scene's root has the tree's Window as its parent.</summary>
    public Node? GetParent() { EnsureLinked(); return _parent; }

    /// <summary>The parent as <typeparamref name="T"/>, or null.</summary>
    public T? GetParent<T>() where T : class => GetParent() as T;

    /// <summary>The scene file this node was instanced from, if it is a scene root.</summary>
    public string SceneFilePath { get => _sceneFilePath; set => _sceneFilePath = value; }

    /// <summary>The node that owns this one (normally the root of the scene it was authored in).</summary>
    public Node? Owner
    {
        get => _owner;
        set
        {
            if (_owner == value) return;
            if (_uniqueNameInOwner) RegisterUnique(false);
            _owner = value;
            if (_uniqueNameInOwner) RegisterUnique(true);
        }
    }

    /// <summary>Whether <c>%Name</c> paths from inside the owner's scene reach this node.</summary>
    public bool UniqueNameInOwner
    {
        get => _uniqueNameInOwner;
        set
        {
            if (_uniqueNameInOwner == value) return;
            if (_uniqueNameInOwner) RegisterUnique(false);
            _uniqueNameInOwner = value;
            if (_uniqueNameInOwner) RegisterUnique(true);
        }
    }

    private void RegisterUnique(bool add)
    {
        if (_owner == null) return;
        _owner._uniqueOwned ??= new();
        if (add) _owner._uniqueOwned[_name] = this;
        else if (_owner._uniqueOwned.TryGetValue(_name, out Node? n) && n == this) _owner._uniqueOwned.Remove(_name);
    }

    public int GetChildCount(bool includeInternal = true) { EnsureChildren(); return _children.Count; }

    public Node GetChild(int idx, bool includeInternal = false)
    {
        EnsureChildren();
        if (idx < 0) idx += _children.Count;
        if (idx < 0 || idx >= _children.Count) throw new ArgumentOutOfRangeException(nameof(idx), $"Child index {idx} is out of range (count {_children.Count}).");
        return _children[idx];
    }

    public T GetChild<T>(int idx, bool includeInternal = false) where T : class => (GetChild(idx, includeInternal) as T)!;

    public Collections.Array<Node> GetChildren(bool includeInternal = false)
    {
        EnsureChildren();
        return new Collections.Array<Node>(_children);
    }

    public bool HasNode(NodePath path) => GetNodeOrNull(path) != null;

    public bool IsAncestorOf(Node node)
    {
        for (Node? p = node.GetParent(); p != null; p = p._parent) if (p == this) return true;
        return false;
    }

    public bool IsGreaterThan(Node node) => TreeOrderCompare(this, node) > 0;

    /// <summary>Adds a child. A name that is empty or taken is replaced ("@Node3D@2"; with <paramref name="forceReadableName"/>, "Enemy2").
    /// If this node is in the tree the child (and its subtree) enters it now: _EnterTree pre-order, then _Ready children first.</summary>
    public void AddChild(Node node, bool forceReadableName = false, InternalMode @internal = InternalMode.Disabled)
    {
        ArgumentNullException.ThrowIfNull(node);
        if (node == this) throw new InvalidOperationException("Can't add a child to itself.");
        if (node._parent != null) throw new InvalidOperationException($"Can't add child '{node.Name}' to '{Name}', it already has a parent '{node._parent.Name}'.");
        if (node.IsAncestorOf(this)) throw new InvalidOperationException("Can't add an ancestor as a child.");
        EnsureChildren();
        node._name = UniqueChildName(node, node._name, forceReadableName);
        node._parent = this;
        _children.Add(node);
        node.OnLinked();
        node._Notification(NotificationParented);
        if (_insideTree) NodeHost.Current.Enter(node);
        EmitSignal(SignalName.ChildOrderChanged);
    }

    /// <summary>Adds a sibling after this node (Godot's add_sibling).</summary>
    public void AddSibling(Node sibling, bool forceReadableName = false)
    {
        Node parent = GetParent() ?? throw new InvalidOperationException("The node has no parent.");
        parent.AddChild(sibling, forceReadableName);
        parent.MoveChild(sibling, GetIndex() + 1);
    }

    /// <summary>Removes a child from this node. It leaves the tree (_ExitTree runs, children first) but is not freed.</summary>
    public void RemoveChild(Node node)
    {
        ArgumentNullException.ThrowIfNull(node);
        EnsureChildren();
        if (node._parent != this) throw new InvalidOperationException($"Node '{node.Name}' is not a child of '{Name}'.");
        if (node._insideTree)
        {
            EmitSignal(SignalName.ChildExitingTree, node);
            NodeHost.Current.Exit(node);
        }
        _children.Remove(node);
        node._parent = null;
        node.OnUnlinked();
        node._Notification(NotificationUnparented);
        if (node._owner != null && !IsInsideOwnerScene(node._owner)) node.Owner = null;
        EmitSignal(SignalName.ChildOrderChanged);
    }

    private bool IsInsideOwnerScene(Node owner) => owner == this || owner.IsAncestorOf(this);

    /// <summary>Moves this node under another parent, keeping its global transform when it is a Node3D (Godot's reparent).</summary>
    public void Reparent(Node newParent, bool keepGlobalTransform = true)
    {
        Node? old = GetParent();
        if (old == null) throw new InvalidOperationException("Can't reparent a node that has no parent.");
        Transform3D? global = keepGlobalTransform && this is Node3D n3 && n3.IsInsideTree() ? n3.GlobalTransform : null;
        old.RemoveChild(this);
        newParent.AddChild(this);
        if (global.HasValue) ((Node3D)this).GlobalTransform = global.Value;
    }

    /// <summary>Moves a child to another position among its siblings.</summary>
    public void MoveChild(Node childNode, int toIndex)
    {
        EnsureChildren();
        if (childNode._parent != this) throw new InvalidOperationException("The node is not a child of this node.");
        if (toIndex < 0) toIndex += _children.Count;
        toIndex = Math.Clamp(toIndex, 0, _children.Count - 1);
        _children.Remove(childNode);
        _children.Insert(toIndex, childNode);
        EmitSignal(SignalName.ChildOrderChanged);
    }

    /// <summary>This node's position among its parent's children.</summary>
    public int GetIndex(bool includeInternal = false) => GetParent()?._children.IndexOf(this) ?? -1;

    /// <summary>Puts a node in this node's place (Godot's replace_by): it takes the children, and this one is removed.</summary>
    public void ReplaceBy(Node node, bool keepGroups = false)
    {
        Node parent = GetParent() ?? throw new InvalidOperationException("The node has no parent.");
        int idx = GetIndex();
        foreach (Node c in _children.ToArray()) { RemoveChild(c); node.AddChild(c); }
        EmitSignal(SignalName.ReplacingBy, node);
        parent.RemoveChild(this);
        parent.AddChild(node);
        parent.MoveChild(node, idx);
    }

    // Picks a name no sibling has. Empty names get "@Class@N"; a taken name gets "@name@N" ("name" + N when readable).
    private string UniqueChildName(Node child, string desired, bool readable)
    {
        bool Taken(string n) => _children.Any(c => c != child && c._name == n);
        if (desired.Length > 0 && !Taken(desired)) return desired;
        if (desired.Length == 0)
        {
            string cls = child.GetClass();
            if (readable && !Taken(cls)) return cls;
            return Serial(readable ? cls : "@" + cls + "@", readable);
        }
        return readable ? Serial(desired.TrimEnd('0', '1', '2', '3', '4', '5', '6', '7', '8', '9'), true) : Serial("@" + desired + "@", false);

        string Serial(string stem, bool plain)
        {
            for (int n = 2; ; n++)
            {
                string cand = plain ? stem + n : stem + n;
                if (!Taken(cand)) return cand;
            }
        }
    }

    // The children as a node sees them: with an entity behind it, entities that are its children but not yet linked are wrapped in.
    private void EnsureChildren()
    {
        if (!Self.IsValid || !NodeHost.Current.IsEngine) return;
        foreach (Entity c in Self.Children)
        {
            Node? n = FromEntity(c);
            if (n == null || n._parent == this) continue;
            if (n._parent == null)
            {
                n._parent = this;
                _children.Add(n);
                n.OnLinked();
            }
        }
    }

    // A node found through its entity (a level-loaded one) learns its parent the first time anything asks.
    private void EnsureLinked()
    {
        if (_parent != null || !Self.IsValid || !NodeHost.Current.IsEngine) return;
        LinkToEntityParent();
    }

    internal void LinkToEntityParent()
    {
        if (_parent != null) return;
        Node? p = Self.Parent.IsValid ? FromEntity(Self.Parent) : null;
        p ??= SceneTree.Instance.Root;
        if (_name.Length == 0) _name = Self.Name;
        _parent = p;
        if (!p._children.Contains(this)) p._children.Add(this);
        OnLinked();
    }

    // A copy of the child list, for walks that may change it.
    internal List<Node> ChildrenSnapshot() => new(_children);

    // Hooks for node types that care about their parent (Node3D's cached global transform, a Control's layout ...).
    internal virtual void OnLinked() { }
    internal virtual void OnUnlinked() { }

    // ---- finding nodes ----

    /// <summary>The node at a path: "Child/Grandchild", "../Sibling", "/root/Main/Child", "%UniqueName", ".". Throws when there is none.</summary>
    public Node GetNode(NodePath path) => GetNodeOrNull(path) ?? throw new InvalidOperationException($"Node not found: \"{path}\" (relative to \"{GetPath2()}\").");

    public T GetNode<T>(NodePath path) where T : class =>
        GetNodeOrNull(path) as T ?? throw new InvalidOperationException($"Node not found or not a {typeof(T).Name}: \"{path}\" (relative to \"{GetPath2()}\").");

    public Node? GetNodeOrNull(NodePath path)
    {
        if (path == null || path.IsEmpty) return null;
        int n = path.GetNameCount();
        Node? current = this;
        Node? root = null;
        if (path.IsAbsolute())
        {
            root = this;
            while (root.GetParent() is Node up) root = up;
            current = null;
        }
        for (int i = 0; i < n; i++)
        {
            string name = path.GetName(i);
            Node? next;
            if (name == ".") next = current;
            else if (name == "..") next = current?.GetParent();
            else if (current == null) next = name == root!._name ? root : null;
            else if (name.Length > 0 && name[0] == '%') next = current.FindUnique(name[1..]);
            else next = current.FindChildByName(name);
            if (next == null) return null;
            current = next;
        }
        return current;
    }

    public T? GetNodeOrNull<T>(NodePath path) where T : class => GetNodeOrNull(path) as T;

    private Node? FindChildByName(string name)
    {
        EnsureChildren();
        foreach (Node c in _children) if (c._name == name) return c;
        return null;
    }

    private Node? FindUnique(string name)
    {
        if (_uniqueOwned != null) return _uniqueOwned.TryGetValue(name, out Node? n) ? n : null;
        if (_owner?._uniqueOwned != null) return _owner._uniqueOwned.TryGetValue(name, out Node? n) ? n : null;
        return null;
    }

    /// <summary>The first descendant whose name matches a wildcard pattern ("*" and "?"), searching recursively unless told not to.</summary>
    public Node? FindChild(string pattern, bool recursive = true, bool owned = true)
    {
        EnsureChildren();
        foreach (Node c in _children)
            if (WildcardMatch(c._name, pattern) && (!owned || c._owner != null)) return c;
        if (recursive)
            foreach (Node c in _children)
                if (c.FindChild(pattern, true, owned) is Node found) return found;
        return null;
    }

    public Collections.Array<Node> FindChildren(string pattern, string type = "", bool recursive = true, bool owned = true)
    {
        var result = new Collections.Array<Node>();
        void Walk(Node n)
        {
            n.EnsureChildren();
            foreach (Node c in n._children)
            {
                if (WildcardMatch(c._name, pattern) && (type.Length == 0 || c.IsClass(type)) && (!owned || c._owner != null)) result.Add(c);
                if (recursive) Walk(c);
            }
        }
        Walk(this);
        return result;
    }

    public Node? FindParent(string pattern)
    {
        for (Node? p = GetParent(); p != null; p = p._parent)
            if (WildcardMatch(p._name, pattern)) return p;
        return null;
    }

    private static bool WildcardMatch(string text, string pattern)
    {
        int t = 0, p = 0, star = -1, mark = 0;
        while (t < text.Length)
        {
            if (p < pattern.Length && (pattern[p] == '?' || char.ToLowerInvariant(pattern[p]) == char.ToLowerInvariant(text[t]))) { t++; p++; }
            else if (p < pattern.Length && pattern[p] == '*') { star = p++; mark = t; }
            else if (star >= 0) { p = star + 1; t = ++mark; }
            else return false;
        }
        while (p < pattern.Length && pattern[p] == '*') p++;
        return p == pattern.Length;
    }

    /// <summary>The path from the tree's root, e.g. "/root/Main/Player".</summary>
    public NodePath GetPath()
    {
        if (!IsInsideTree()) throw new InvalidOperationException("Cannot get the path of a node that is not in the tree.");
        return new NodePath(GetPath2());
    }

    // The path, also for a node outside the tree (up to the highest ancestor).
    private string GetPath2()
    {
        var parts = new List<string>();
        for (Node? n = this; n != null; n = n._parent) parts.Add(n._name);
        parts.Reverse();
        return (IsInsideTree() ? "/" : "") + string.Join('/', parts);
    }

    /// <summary>The path from this node to another in the same tree ("../Sibling").</summary>
    public NodePath GetPathTo(Node node, bool useUniquePath = false)
    {
        var mine = new List<Node>();
        for (Node? n = this; n != null; n = n._parent) mine.Add(n);
        var theirs = new List<Node>();
        for (Node? n = node; n != null; n = n._parent) theirs.Add(n);
        mine.Reverse(); theirs.Reverse();
        int common = 0;
        while (common < mine.Count && common < theirs.Count && mine[common] == theirs[common]) common++;
        if (common == 0) return new NodePath("");
        var segs = new List<string>();
        for (int i = common; i < mine.Count; i++) segs.Add("..");
        for (int i = common; i < theirs.Count; i++) segs.Add(theirs[i]._name);
        return new NodePath(segs.Count == 0 ? "." : string.Join('/', segs));
    }

    // Compares two nodes by position in the tree (depth first, siblings in order); negative when a comes first.
    internal static int TreeOrderCompare(Node a, Node b)
    {
        if (a == b) return 0;
        var pa = PathOfIndices(a); var pb = PathOfIndices(b);
        for (int i = 0; i < Math.Min(pa.Count, pb.Count); i++)
            if (pa[i] != pb[i]) return pa[i].CompareTo(pb[i]);
        return pa.Count.CompareTo(pb.Count);
    }

    private static List<int> PathOfIndices(Node n)
    {
        var list = new List<int>();
        for (Node? c = n; c?._parent != null; c = c._parent) list.Add(c._parent._children.IndexOf(c));
        list.Reverse();
        return list;
    }

    // ---- groups ----

    private HashSet<string>? _groups;
    private static readonly Dictionary<string, List<Node>> s_groupMembers = new();

    /// <summary>Puts the node in a named group. Groups of a node loaded from a level are read when it enters the tree.</summary>
    public void AddToGroup(StringName group, bool persistent = false)
    {
        string g = group.ToString();
        if (g.Length == 0) return;
        _groups ??= new();
        if (!_groups.Add(g)) return;
        lock (s_groupMembers)
        {
            if (!s_groupMembers.TryGetValue(g, out var list)) s_groupMembers[g] = list = new List<Node>();
            list.Add(this);
        }
        if (Self.IsValid && NodeHost.Current.IsEngine) Groups.Add(Self, g);
    }

    public void RemoveFromGroup(StringName group)
    {
        string g = group.ToString();
        if (_groups == null || !_groups.Remove(g)) return;
        lock (s_groupMembers) { if (s_groupMembers.TryGetValue(g, out var list)) list.Remove(this); }
        if (Self.IsValid && NodeHost.Current.IsEngine) Groups.Remove(Self, g);
    }

    public bool IsInGroup(StringName group) => _groups != null && _groups.Contains(group.ToString());

    public Collections.Array<StringName> GetGroups() => new(_groups?.Select(g => new StringName(g)) ?? Enumerable.Empty<StringName>());

    // The nodes in a group in tree order; groups of nodes not in the tree do not count.
    internal static List<Node> NodesInGroup(string group)
    {
        List<Node> copy;
        lock (s_groupMembers) copy = s_groupMembers.TryGetValue(group, out var list) ? list.Where(n => n._insideTree && n.IsAlive).ToList() : new List<Node>();
        copy.Sort(TreeOrderCompare);
        return copy;
    }

    internal static void ResetGroups()
    {
        lock (s_groupMembers) s_groupMembers.Clear();
    }

    internal static bool GroupExists(string group)
    {
        lock (s_groupMembers) return s_groupMembers.TryGetValue(group, out var list) && list.Any(n => n._insideTree && n.IsAlive);
    }

    private void LeaveAllGroups()
    {
        if (_groups == null) return;
        lock (s_groupMembers) foreach (string g in _groups) if (s_groupMembers.TryGetValue(g, out var list)) list.Remove(this);
    }

    // ---- deferred destruction ----

    /// <summary>Frees the node and its descendants at the end of the frame (Godot's queue_free).</summary>
    public void QueueFree()
    {
        if (IsQueuedForDeletion() || !IsAlive) return;
        MarkQueuedForDeletion();
        Deferred.Call(() => { if (IsAlive) Free(); });
    }

    /// <summary>Frees the node and its descendants now.</summary>
    public override void Free()
    {
        if (!IsAlive) return;
        Node? parent = _parent;
        if (parent != null && _insideTree)
        {
            parent.EmitSignal(SignalName.ChildExitingTree, this);
        }
        NodeHost.Current.Destroy(this);
        if (parent != null) { parent._children.Remove(this); parent.EmitSignal(SignalName.ChildOrderChanged); }
        _parent = null;
        FreeSubtreeMarks();
    }

    // Marks this node and everything below it freed and out of its groups; the host has already run the exit notifications.
    internal void FreeSubtreeMarks()
    {
        foreach (Node c in _children.ToArray()) c.FreeSubtreeMarks();
        _children.Clear();
        LeaveAllGroups();
        if (_uniqueNameInOwner) RegisterUnique(false);
        base.Free();
    }

    /// <summary>Copies this node and its descendants: same class, name, properties and children; groups and signals as the flags say.</summary>
    public Node Duplicate(int flags = 15)
    {
        Node copy = (Node)Activator.CreateInstance(GetType())!;
        copy._name = _name;
        copy._sceneFilePath = _sceneFilePath;
        foreach (System.Reflection.FieldInfo f in GetType().GetFields(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic))
        {
            if (f.DeclaringType!.Assembly == typeof(Node).Assembly || f.IsInitOnly) continue;
            if (f.FieldType.IsSubclassOf(typeof(Node)) || f.FieldType == typeof(Node)) continue;
            f.SetValue(copy, f.GetValue(this));
        }
        CopyEngineState(copy);
        if ((flags & (int)DuplicateFlags.Groups) != 0 && _groups != null) foreach (string g in _groups) copy.AddToGroup(g);
        EnsureChildren();
        foreach (Node c in _children) copy.AddChild(c.Duplicate(flags));
        return copy;
    }

    internal virtual void CopyEngineState(Node copy) { }

    public T Duplicate<T>(int flags = 15) where T : Node => (T)Duplicate(flags);

    // ---- the tree ----

    private bool _persistent;

    /// <summary>True for an autoload: it survives a scene change (Game.ChangeLevel), with everything below it. Kept in the node until
    /// it has an entity, then in the engine.</summary>
    public new bool Persistent
    {
        get => Self.IsValid && NodeHost.Current.IsEngine ? base.Persistent : _persistent;
        set
        {
            _persistent = value;
            if (Self.IsValid && NodeHost.Current.IsEngine) base.Persistent = value;
        }
    }

    /// <summary>The scene tree this node is in, or null when it is not in one.</summary>
    public SceneTree? GetTree() => _insideTree ? SceneTree.Instance : null;

    /// <summary>The window at the top of the tree.</summary>
    public Viewport? GetViewport() => _insideTree ? SceneTree.Instance.Root : null;

    public Window? GetWindow() => _insideTree ? SceneTree.Instance.Root : null;

    /// <summary>A tween bound to this node: it stops when the node is freed, waits while the node is out of the tree, and pauses with it.</summary>
    public Tween CreateTween() => SceneTree.Instance.CreateTween().BindNode(this);

    public bool IsInsideTree() => _insideTree;

    /// <summary>Whether _Ready has run for this node.</summary>
    public bool IsNodeReady() => _readyDone;
}
