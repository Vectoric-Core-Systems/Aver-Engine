// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// NodeHost: what happens to a subtree of nodes when it enters the tree, leaves it, or is freed. The engine host gives each node an
// entity and lets the framework drive the lifecycle; the managed host drives it itself (headless tests, tools).

using Aver.Framework;
using Aver.Scripting;

namespace Aver.Fabric;

internal interface INodeHost
{
    /// <summary>True when nodes live in the engine's world (they have entities).</summary>
    bool IsEngine { get; }

    /// <summary>A subtree was added under a parent that is in the tree.</summary>
    void Enter(Node subtreeRoot);

    /// <summary>A subtree is being removed from the tree (it stays alive).</summary>
    void Exit(Node subtreeRoot);

    /// <summary>A node is being freed now: its subtree leaves the tree and its entities go.</summary>
    void Destroy(Node node);
}

internal static class NodeHost
{
    private static INodeHost? s_current;

    /// <summary>The host in use. The engine host unless a test or tool installed the managed one.</summary>
    internal static INodeHost Current
    {
        get => s_current ??= new EngineNodeHost();
        set => s_current = value;
    }

    internal static IEnumerable<Node> PreOrder(Node n)
    {
        yield return n;
        foreach (Node c in n.ChildrenSnapshot())
            foreach (Node d in PreOrder(c)) yield return d;
    }

    internal static IEnumerable<Node> PostOrder(Node n)
    {
        foreach (Node c in n.ChildrenSnapshot())
            foreach (Node d in PostOrder(c)) yield return d;
        yield return n;
    }

    // Godot exits a subtree last child first.
    internal static IEnumerable<Node> ExitOrder(Node n)
    {
        List<Node> kids = n.ChildrenSnapshot();
        for (int i = kids.Count - 1; i >= 0; i--)
            foreach (Node d in ExitOrder(kids[i])) yield return d;
        yield return n;
    }

    internal static void ManagedEnter(Node root)
    {
        foreach (Node n in PreOrder(root).ToList()) n.NotifyEnterTree();
        foreach (Node n in PostOrder(root).ToList()) n.NotifyReady();
    }

    internal static void ManagedExit(Node root)
    {
        foreach (Node n in ExitOrder(root).ToList()) n.NotifyExitTree();
    }
}

/// <summary>Runs the lifecycle itself, with no engine: what the framework's spawn batch does, in Godot's order.</summary>
internal sealed class ManagedNodeHost : INodeHost
{
    public bool IsEngine => false;
    public void Enter(Node subtreeRoot) => NodeHost.ManagedEnter(subtreeRoot);
    public void Exit(Node subtreeRoot) => NodeHost.ManagedExit(subtreeRoot);
    public void Destroy(Node node) => NodeHost.ManagedExit(node);
}

/// <summary>Nodes are entities in the Aver world. A node made with <c>new</c> becomes an actor of its class when it enters the tree;
/// the framework then runs _EnterTree and _Ready in Godot's order (a spawn batch).</summary>
internal sealed class EngineNodeHost : INodeHost
{
    public bool IsEngine => true;

    public void Enter(Node subtreeRoot)
    {
        Entity parentEntity = subtreeRoot.GetParent()?.Self ?? default;
        bool anyFresh = NodeHost.PreOrder(subtreeRoot).Any(n => !n.Self.IsValid);
        if (anyFresh) Aver.Framework.Hosting.ExtensionHost.BeginSpawnBatch();
        try
        {
            foreach (Node n in NodeHost.PreOrder(subtreeRoot).ToList())
            {
                if (n.Self.IsValid)
                {
                    // Already an entity (it was removed and is being added again): only its parent changes.
                    if (n == subtreeRoot && parentEntity.IsValid) n.Self.SetParent(parentEntity);
                    else if (n == subtreeRoot) n.Self.Detach();
                    continue;
                }
                SpawnOne(n);
            }
        }
        finally
        {
            if (anyFresh) Aver.Framework.Hosting.ExtensionHost.EndSpawnBatch();
        }
        // Nodes that were already entities do not go through the framework's spawn path; notify them here.
        var reentering = NodeHost.PreOrder(subtreeRoot).Where(n => n.Self.IsValid && !n.IsInsideTree()).ToList();
        foreach (Node n in reentering) n.NotifyEnterTree();
        foreach (Node n in NodeHost.PostOrder(subtreeRoot).Where(reentering.Contains).ToList()) n.NotifyReady();
    }

    public void Exit(Node subtreeRoot)
    {
        NodeHost.ManagedExit(subtreeRoot);
        if (subtreeRoot.Self.IsValid) subtreeRoot.Self.Detach();
    }

    public void Destroy(Node node)
    {
        if (node.Self.IsValid && node.Self.IsAlive) node.Self.Destroy();   // the framework ends every actor below, children first
        else NodeHost.ManagedExit(node);
    }

    private static void SpawnOne(Node n)
    {
        ActorClass cls = default;
        for (System.Type? t = n.GetType(); t != null && !cls.IsValid; t = t.BaseType)
            cls = ActorClass.Find(ClassNames.Of(t));
        if (!cls.IsValid)
        {
            Log.Error($"[Fabric] {n.GetType().Name} '{n.Name}' has no class in the registry; it was not added to the world");
            return;
        }
        Entity parent = n.GetParent()?.Self ?? default;
        float[] pos = { 0, 0, 0 }, quat = { 0, 0, 0, 1 }, scale = { 1, 1, 1 };
        if (n is Node3D n3) n3.GetLocalForEngine(pos, quat, scale);

        int e = Aver.Framework.Hosting.ExtensionHost.SpawnActor(cls, n.Name.ToString(), pos, quat, scale, n);
        if (e == 0)
        {
            Log.Error($"[Fabric] {n.GetType().Name} '{n.Name}' could not be spawned");
            return;
        }
        var entity = Aver.Framework.Hosting.ExtensionHost.EntityFromHandle(e);
        if (n is Node3D spawned) spawned.MarkSpawned();
        if (parent.IsValid) entity.SetParent(parent);
        if (!entity.IsBoundTo(n)) Log.Error($"[Fabric] the scripting bridge did not bind the {n.GetType().Name} '{n.Name}' it was handed");
    }
}

internal static class EntityBindCheck
{
    // True when the actor bound to the entity is exactly this node.
    internal static bool IsBoundTo(this Entity entity, Node node) => ReferenceEquals(Actors.Get(entity), node);
}
