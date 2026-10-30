// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Groups: named groups of entities, kept natively so a
// level can carry them (COMPONENT Groups names=...).

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Named groups of entities, actor or not, by case-sensitive name. A destroyed entity leaves every group.</summary>
public static class Groups
{
    /// <summary>Puts the entity in the group. False when it already was.</summary>
    public static bool Add(Entity e, string group) => Fw.aver_fw_group_add(e.Handle, group) != 0;

    /// <summary>Takes the entity out of the group. False when it was not in it.</summary>
    public static bool Remove(Entity e, string group) => Fw.aver_fw_group_remove(e.Handle, group) != 0;

    /// <summary>Whether the entity is in the group.</summary>
    public static bool Has(Entity e, string group) => Fw.aver_fw_group_has(e.Handle, group) != 0;

    /// <summary>The group's members in scene-tree order (a parent before its children, siblings in the order added).</summary>
    public static Entity[] Members(string group)
    {
        int n = Fw.aver_fw_group_count(group);
        var handles = new int[n];
        for (int i = 0; i < n; ++i) handles[i] = Fw.aver_fw_group_member(group, i);
        var order = new int[n];
        if (n > 1 && Fw.aver_fw_tree_order(handles, new int[n], n, order) != 0)
            return order.Select(i => new Entity(handles[i])).ToArray();
        return handles.Select(h => new Entity(h)).ToArray();
    }

    /// <summary>The groups the entity is in, in the order it joined them.</summary>
    public static string[] Of(Entity e)
    {
        int n = Fw.aver_fw_groups_of_count(e.Handle);
        var names = new string[n];
        for (int i = 0; i < n; ++i) names[i] = Marshal.PtrToStringUTF8(Fw.aver_fw_groups_of(e.Handle, i)) ?? "";
        return names;
    }

    /// <summary>Calls <paramref name="action"/> on every actor of type <typeparamref name="T"/> in the group, in tree
    /// order. Members that are not such an actor are skipped. Returns how many it called.</summary>
    public static int Call<T>(string group, Action<T> action) where T : AverActor
    {
        int n = 0;
        foreach (Entity e in Members(group))
            if (Actors.Get<T>(e) is T a) { action(a); ++n; }
        return n;
    }
}

public abstract partial class AverActor
{
    /// <summary>Puts this actor in a named group.</summary>
    public bool AddToGroup(string group) => Groups.Add(Self, group);

    /// <summary>Takes this actor out of a named group.</summary>
    public bool RemoveFromGroup(string group) => Groups.Remove(Self, group);

    /// <summary>Whether this actor is in a named group.</summary>
    public bool IsInGroup(string group) => Groups.Has(Self, group);
}
