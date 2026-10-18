// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The SceneQuery rows of the node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2). A graph has no array
// pin, so a set is read as a count plus an item at an index; the public SceneQueries class is the same thing for C#.
// Every query is a linear scan of the live scene: cheap for a handful of calls a frame, not for one per entity.

namespace Aver.Framework;

/// <summary>Counted access to sets of entities: by tag, by actor class, and a parent's children.</summary>
public static class SceneQueries
{
    /// <summary>How many live entities carry every bit of <paramref name="mask"/> (zero matches none).</summary>
    public static int CountWithTag(uint mask)
    {
        int n = 0;
        foreach (Entity _ in Game.WithTag(mask)) n++;
        return n;
    }

    /// <summary>The <paramref name="index"/>th live entity carrying every bit of <paramref name="mask"/>, in scene order,
    /// or <see cref="Entity.None"/> when the index is out of range.</summary>
    public static Entity WithTagAt(uint mask, int index)
    {
        if (index < 0) return Entity.None;
        int i = 0;
        foreach (Entity e in Game.WithTag(mask))
            if (i++ == index) return e;
        return Entity.None;
    }

    /// <summary>How many live actors are of class <paramref name="className"/> or a class derived from it.</summary>
    public static int CountActorsOfClass(string className)
    {
        int n = 0;
        foreach (Entity _ in Game.ActorsOfClass(className)) n++;
        return n;
    }

    /// <summary>The <paramref name="index"/>th live actor of the class (derived classes included), or
    /// <see cref="Entity.None"/> when the index is out of range or the class is unknown.</summary>
    public static Entity ActorOfClassAt(string className, int index)
    {
        if (index < 0) return Entity.None;
        int i = 0;
        foreach (Entity e in Game.ActorsOfClass(className))
            if (i++ == index) return e;
        return Entity.None;
    }

    /// <summary>How many live actors exist; plain scene entities are not counted.</summary>
    public static int ActorCount()
    {
        int n = 0;
        foreach (Entity _ in Game.AllActors()) n++;
        return n;
    }

    /// <summary>The <paramref name="index"/>th immediate child of <paramref name="parent"/> in link order, or
    /// <see cref="Entity.None"/> when the index is out of range.</summary>
    public static Entity ChildAt(Entity parent, int index)
    {
        if (index < 0) return Entity.None;
        int i = 0;
        for (Entity c = parent.FirstChild; c.IsValid; c = c.NextSibling)
            if (i++ == index) return c;
        return Entity.None;
    }

    /// <summary>The world-space distance in centimetres between two entities. False (and 0) when either is not alive.</summary>
    public static bool TryGetDistance(Entity a, Entity b, out float distance)
    {
        distance = 0f;
        if (!a.IsAlive || !b.IsAlive) return false;
        distance = Aver.Scene.Vec3.Distance(a.WorldPosition, b.WorldPosition);
        return true;
    }

    /// <summary>The entity's ragdoll mode as an int (0 Kinematic, 1 Limp, 2 Powered), or -1 when it has none.</summary>
    public static int RagdollModeOf(Entity e) => e.RagdollState is { } m ? (int)m : -1;
}

/// <summary>The per-node wrappers behind the SceneQuery rows: handles as ints, failures give 0 or false and log once.</summary>
internal static class GraphSceneQueries
{
    /// <summary>How many entities carry every bit of the tag mask.</summary>
    internal static int CountActorsWithTagForGraph(int mask)
    {
        try { return SceneQueries.CountWithTag(unchecked((uint)mask)); }
        catch (Exception e) { FlowGuard.Failed("CountActorsWithTag", e); return 0; }
    }

    /// <summary>The entity at an index among those carrying the tag mask; 0 out of range.</summary>
    internal static int GetActorWithTagAtForGraph(int mask, int index)
    {
        try { return SceneQueries.WithTagAt(unchecked((uint)mask), index).Handle; }
        catch (Exception e) { FlowGuard.Failed("GetActorWithTagAt", e); return 0; }
    }

    /// <summary>How many live actors are of the named class or a class derived from it.</summary>
    internal static int CountActorsOfClassForGraph(string className)
    {
        try { return SceneQueries.CountActorsOfClass(className); }
        catch (Exception e) { FlowGuard.Failed("CountActorsOfClass", e); return 0; }
    }

    /// <summary>The actor at an index among those of the named class; 0 out of range.</summary>
    internal static int GetActorOfClassAtForGraph(string className, int index)
    {
        try { return SceneQueries.ActorOfClassAt(className, index).Handle; }
        catch (Exception e) { FlowGuard.Failed("GetActorOfClassAt", e); return 0; }
    }

    /// <summary>How many immediate children an entity has; 0 for a dead handle.</summary>
    internal static int GetChildCountForGraph(int entity)
    {
        try { return new Entity(entity).ChildCount; }
        catch (Exception e) { FlowGuard.Failed("GetChildCount", e); return 0; }
    }

    /// <summary>The child at an index in link order; 0 out of range.</summary>
    internal static int GetChildAtForGraph(int entity, int index)
    {
        try { return SceneQueries.ChildAt(new Entity(entity), index).Handle; }
        catch (Exception e) { FlowGuard.Failed("GetChildAt", e); return 0; }
    }

    /// <summary>An entity's parent; 0 for a root or a dead handle.</summary>
    internal static int GetParentForGraph(int entity)
    {
        try { return new Entity(entity).Parent.Handle; }
        catch (Exception e) { FlowGuard.Failed("GetParent", e); return 0; }
    }

    /// <summary>True when the entity carries the named component (for example CLight).</summary>
    internal static bool HasComponentForGraph(int entity, string component)
    {
        try { return !string.IsNullOrEmpty(component) && new Entity(entity).HasComponent(component); }
        catch (Exception e) { FlowGuard.Failed("HasComponent", e); return false; }
    }

    /// <summary>Attaches the named component, zero-filled. False for a dead entity or an unregistered name.</summary>
    internal static bool AddComponentForGraph(int entity, string component)
    {
        try
        {
            Entity e = new(entity);
            return !string.IsNullOrEmpty(component) && e.IsAlive && e.AddComponent(component);
        }
        catch (Exception ex) { FlowGuard.Failed("AddComponent", ex); return false; }
    }

    /// <summary>The world distance in centimetres between two entities; false when either is dead.</summary>
    internal static bool GetDistanceForGraph(int a, int b, out float distance)
    {
        distance = 0f;
        try { return SceneQueries.TryGetDistance(new Entity(a), new Entity(b), out distance); }
        catch (Exception e) { FlowGuard.Failed("GetDistance", e); distance = 0f; return false; }
    }

    /// <summary>The ragdoll mode (-1 none, 0 Kinematic, 1 Limp, 2 Powered); the result is whether the entity has a ragdoll.</summary>
    internal static bool GetRagdollStateForGraph(int entity, out int mode)
    {
        mode = -1;
        try
        {
            mode = SceneQueries.RagdollModeOf(new Entity(entity));
            return mode >= 0;
        }
        catch (Exception e) { FlowGuard.Failed("GetRagdollState", e); mode = -1; return false; }
    }

    /// <summary>How many live actors exist.</summary>
    internal static int GetActorCountForGraph()
    {
        try { return SceneQueries.ActorCount(); }
        catch (Exception e) { FlowGuard.Failed("GetActorCount", e); return 0; }
    }
}
