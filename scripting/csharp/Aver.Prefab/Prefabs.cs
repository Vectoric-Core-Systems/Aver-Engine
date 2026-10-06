// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The game-author-facing prefab API, and the seam the AN_ prefab nodes' compiled IL calls.

namespace Aver.Prefab;

/// <summary>Spawn and query prefab instances. Entities are the same int handles <c>Aver.Scene</c> uses.</summary>
/// <remarks>
/// Every call returns 0 (or false) until the host has installed its prefab system, which the editor and the
/// runtime do at start-up. A prefab is named by its content-relative path, <c>"Prefabs/Crate.ocprefab"</c>.
/// </remarks>
public static class Prefabs
{
    /// <summary>The ABI version the native library was built with: (major &lt;&lt; 16) | minor.</summary>
    public static int AbiVersion => Native.aver_prefab_abi_version();

    /// <summary>Spawns an instance and returns its root entity, 0 on failure.</summary>
    /// <param name="path">The prefab asset, relative to the project's Content folder.</param>
    /// <param name="parent">The entity to parent it to, 0 for none.</param>
    /// <param name="yaw">Degrees about the up axis; <paramref name="pitch"/> and <paramref name="roll"/> likewise.</param>
    /// <param name="scale">Uniform; 0 is read as 1.</param>
    public static int Spawn(string path, float x, float y, float z, float yaw = 0f, float pitch = 0f,
                            float roll = 0f, float scale = 1f, int parent = 0)
        => string.IsNullOrEmpty(path) ? 0 : Native.aver_prefab_spawn(path, parent, x, y, z, yaw, pitch, roll, scale);

    /// <summary>Destroys the instance <paramref name="root"/> and everything under it.</summary>
    public static bool Destroy(int root) => root != 0 && Native.aver_prefab_destroy(root) != 0;

    /// <summary>The root of the instance <paramref name="entity"/> belongs to; 0 when it is not part of one.</summary>
    public static int RootOf(int entity) => entity == 0 ? 0 : Native.aver_prefab_root_of(entity);

    /// <summary>True when <paramref name="entity"/> is part of a prefab instance.</summary>
    public static bool IsInstance(int entity) => entity != 0 && Native.aver_prefab_is_instance(entity) != 0;

    /// <summary>The entity at <paramref name="nodePath"/> inside an instance: "" is the root, "5" a node, "3/5" a node of a nested prefab.</summary>
    public static int Find(int root, string nodePath = "") => root == 0 ? 0 : Native.aver_prefab_find(root, nodePath ?? "");

    /// <summary>Puts every field the instance changed back to what the prefab says.</summary>
    public static bool Revert(int root) => root != 0 && Native.aver_prefab_revert(root) != 0;

    /// <summary>How many fields, components and names of the instance differ from the prefab.</summary>
    public static int OverrideCount(int root) => root == 0 ? 0 : Native.aver_prefab_override_count(root);
}

/// <summary>
/// Scalar-signature seam the AN_SpawnPrefab / AN_DestroyPrefab / AN_GetPrefabRoot / AN_FindPrefabNode /
/// AN_RevertPrefab nodes' compiled IL calls (the shape GraphTimerEvents has for the timer nodes). The
/// asset path and node path are node ATTRIBUTES (`prefab=` and `node=`), not pins, because a graph pin
/// is float, int or bool. Wiring: docs/PREFABS.md.
/// </summary>
public static class GraphPrefabs
{
    /// <summary>The node type names, for the parser and the catalog to share.</summary>
    public const string SpawnNodeType = "AN_SpawnPrefab";
    public const string DestroyNodeType = "AN_DestroyPrefab";
    public const string RootNodeType = "AN_GetPrefabRoot";
    public const string FindNodeType = "AN_FindPrefabNode";
    public const string RevertNodeType = "AN_RevertPrefab";

    /// <summary>AN_SpawnPrefab: the new instance's root, 0 on failure.</summary>
    public static int SpawnPrefabForGraph(int parent, float x, float y, float z, float yaw, float scale, string prefab)
        => Prefabs.Spawn(prefab, x, y, z, yaw, 0f, 0f, scale, parent);

    /// <summary>AN_DestroyPrefab.</summary>
    public static bool DestroyPrefabForGraph(int root) => Prefabs.Destroy(root);

    /// <summary>AN_GetPrefabRoot: the root of the instance the entity is part of, 0 if none.</summary>
    public static int PrefabRootForGraph(int entity) => Prefabs.RootOf(entity);

    /// <summary>AN_FindPrefabNode: the entity at the node path inside the instance, 0 if none.</summary>
    public static int FindPrefabNodeForGraph(int root, string node) => Prefabs.Find(root, node);

    /// <summary>AN_RevertPrefab.</summary>
    public static bool RevertPrefabForGraph(int root) => Prefabs.Revert(root);
}
