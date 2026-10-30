// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Fabric/nodes.json, read: the nodes of every scene as the scene converter placed them. A script attached to a node can then type $Path,
// get_node("path") and %Unique from the scene instead of calling what it finds by name (docs/plans/fabric/G3_SCRIPTS.md).

using System.Text.Json;

namespace Aver.Fabric.Convert.GdScript;

/// <summary>One node of a scene.</summary>
internal sealed class SceneNode
{
    public string Path = ".";
    public string Base = "";                 // the Aver.Fabric class placed
    public string Class = "";                // the class named in the level (the script's class when one is attached)
    public string Script = "";
    public string Owner = "";                // "" for the scene root
    public bool Unique;
    public Dictionary<string, string> Packed = new(StringComparer.Ordinal);   // property -> res:// scene
}

/// <summary>The nodes of one scene, by path from its root.</summary>
internal sealed class SceneDoc
{
    public string Scene = "";
    public readonly List<SceneNode> Nodes = new();
    public readonly Dictionary<string, SceneNode> ByPath = new(StringComparer.Ordinal);
}

/// <summary>A node in a scene: where a script is attached, or where a path led.</summary>
internal readonly record struct ScenePlace(SceneDoc Doc, SceneNode Node);

internal sealed class SceneIndex
{
    private readonly List<SceneDoc> _docs = new();
    private readonly Dictionary<string, SceneDoc> _byScene = new(StringComparer.Ordinal);

    public static SceneIndex? Parse(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            if (!doc.RootElement.TryGetProperty("scenes", out JsonElement scenes) || scenes.ValueKind != JsonValueKind.Array) return null;
            var index = new SceneIndex();
            foreach (JsonElement s in scenes.EnumerateArray())
            {
                var scene = new SceneDoc { Scene = Str(s, "scene") };
                if (s.TryGetProperty("nodes", out JsonElement nodes) && nodes.ValueKind == JsonValueKind.Array)
                    foreach (JsonElement n in nodes.EnumerateArray())
                    {
                        var node = new SceneNode
                        {
                            Path = Str(n, "path"), Base = Str(n, "base"), Class = Str(n, "class"), Script = Str(n, "script"), Owner = Str(n, "owner"),
                            Unique = n.TryGetProperty("unique", out JsonElement u) && u.ValueKind == JsonValueKind.True,
                        };
                        if (n.TryGetProperty("packed", out JsonElement packed) && packed.ValueKind == JsonValueKind.Object)
                            foreach (JsonProperty p in packed.EnumerateObject()) node.Packed[p.Name] = p.Value.GetString() ?? "";
                        scene.Nodes.Add(node);
                        scene.ByPath[node.Path] = node;
                    }
                index._docs.Add(scene);
                index._byScene[scene.Scene] = scene;
            }
            return index;
        }
        catch (JsonException) { return null; }
    }

    private static string Str(JsonElement e, string name) => e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    /// <summary>Every node whose script is one of <paramref name="scripts"/> (res:// paths).</summary>
    public IEnumerable<ScenePlace> Placements(ICollection<string> scripts)
    {
        foreach (SceneDoc d in _docs)
            foreach (SceneNode n in d.Nodes)
                if (n.Script.Length > 0 && scripts.Contains(n.Script)) yield return new ScenePlace(d, n);
    }

    /// <summary>The root of a scene written on its own, or null.</summary>
    public ScenePlace? Root(string scene) =>
        _byScene.TryGetValue(scene, out SceneDoc? d) && d.ByPath.TryGetValue(".", out SceneNode? r) ? new ScenePlace(d, r) : null;

    /// <summary>Where a node path (as get_node takes it: "A/B", "../C", "%Name/D", ".") leads from a node, or null when it leads nowhere known.</summary>
    public static ScenePlace? Resolve(ScenePlace from, string path)
    {
        if (path.Length == 0 || path[0] == '/') return null;
        ScenePlace cur = from;
        foreach (string seg in path.Split('/'))
        {
            if (seg.Length == 0 || seg == ".") continue;
            if (seg == "..")
            {
                if (cur.Node.Path == ".") return null;
                int cut = cur.Node.Path.LastIndexOf('/');
                string parent = cut < 0 ? "." : cur.Node.Path[..cut];
                if (!cur.Doc.ByPath.TryGetValue(parent, out SceneNode? up)) return null;
                cur = new ScenePlace(cur.Doc, up);
            }
            else if (seg[0] == '%')
            {
                SceneNode? hit = Unique(cur, seg[1..]);
                if (hit == null) return null;
                cur = new ScenePlace(cur.Doc, hit);
            }
            else
            {
                string child = cur.Node.Path == "." ? seg : cur.Node.Path + "/" + seg;
                if (!cur.Doc.ByPath.TryGetValue(child, out SceneNode? next)) return null;
                cur = new ScenePlace(cur.Doc, next);
            }
        }
        return cur;
    }

    // A unique name is looked up among the nodes the caller owns (a scene root owns its scene), else among those its owner owns.
    private static SceneNode? Unique(ScenePlace from, string name)
    {
        foreach (string scope in new[] { from.Node.Path, from.Node.Owner })
        {
            if (scope.Length == 0) continue;
            foreach (SceneNode n in from.Doc.Nodes)
                if (n.Unique && n.Owner == scope && LeafName(n.Path) == name) return n;
        }
        return null;
    }

    private static string LeafName(string path) => path.Contains('/') ? path[(path.LastIndexOf('/') + 1)..] : path;
}
