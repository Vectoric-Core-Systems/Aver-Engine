// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Typed node access: $Path, get_node("path"), %Unique and PackedScene.instantiate() take their type from the scenes the script is attached to
// (Fabric/nodes.json, what the scene converter placed), so a call on the node is a typed member call and not a call by name.

using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private readonly Dictionary<ScriptInfo, List<ScenePlace>> _placements = new();

    /// <summary>The nodes of the scenes this script (or a script derived from it) is attached to; null when no scene is known.</summary>
    private List<ScenePlace>? SelfPlaces() => PlacementsOf(_script);

    private List<ScenePlace>? PlacementsOf(ScriptInfo script)
    {
        if (_m.Scenes == null) return null;
        ScriptInfo top = script.Root;
        if (!_placements.TryGetValue(top, out List<ScenePlace>? list))
        {
            var paths = new HashSet<string>(StringComparer.Ordinal);
            foreach (ScriptInfo s in _m.Scripts)
                if (s.DerivesFromScript(top)) paths.Add(s.ResPath);
            _placements[top] = list = _m.Scenes.Placements(paths).ToList();
        }
        return list.Count > 0 ? list : null;
    }

    // ---- the script's own word for a path ----
    // A node the scenes do not list (inside an imported model, say) can still be typed by the script: `$Path as Node3D` or `var n: Node3D = $Path`
    // anywhere in it says what the path is. Two different words for one path say nothing.

    private readonly Dictionary<ScriptInfo, Dictionary<string, GdType?>> _castPaths = new();

    private GdType? CastPathType(string path)
    {
        ScriptInfo top = _script.Root;
        if (!_castPaths.TryGetValue(top, out Dictionary<string, GdType?>? map))
        {
            map = new Dictionary<string, GdType?>(StringComparer.Ordinal);
            void Add(Expr value, TypeRef type)
            {
                string? p = value switch
                {
                    GetNodeExpr g => g.Path,
                    Call { Callee: Identifier { Name: "get_node" }, Args: [Literal { Kind: LitKind.String or LitKind.NodePath or LitKind.StringName } lit] } => (string)lit.Value!,
                    _ => null,
                };
                if (p == null) return;
                GdType t = ResolveType(type, top);
                if (t.Kind is not (GK.Object or GK.ScriptClass) || t.Sym?.Name == "GodotObject") t = GdType.Variant;
                map[p] = !map.TryGetValue(p, out GdType? prior) ? (t.IsVariant ? null : t) : (prior != null && prior.Cs == t.Cs ? prior : null);
            }
            foreach (Node n in top.Decl.Descendants())
            {
                if (n is Cast c) Add(c.Value, c.Type);
                else if (n is VarDecl { Type: not null, Init: not null } vd) Add(vd.Init, vd.Type);
            }
            _castPaths[top] = map;
        }
        return map.GetValueOrDefault(path);
    }

    private Ex? CastPathEx(string path, bool orNull)
    {
        if (!ScriptIsNode() || CastPathType(path) is not { } t) return null;
        _ctx.UsedInstance = true;
        return new Ex($"{(orNull ? "GetNodeOrNull" : "GetNode")}<{t.Cs}>({Naming.Quote(path)})", t.AsScene()) { UsesInstance = true };
    }

    // A variable typed from a scene is that node where it was found; one that is assigned again may hold another kind of node, so it stays a Node.
    private GdType GuardScene(GdType inferred, string name, Node scope) =>
        inferred.FromScene && AssignsTo(scope, name) ? Obj("Node") : inferred;

    private bool ScriptIsNode()
    {
        Rs.INamedTypeSymbol? node = Api?.Fabric("Node");
        return _script.ApiRoot is { } root && node != null && GdType.Derives(root, node);
    }

    private GdType? PlaceType(ScenePlace p)
    {
        SceneNode n = p.Node;
        if (n.Script.Length > 0 && _m.ByPath.TryGetValue(n.Script, out ScriptInfo? si)) return GdType.OfScript(si);
        if (Api?.Fabric(n.Base) is { } sym && ApiModel.IsGodotDerived(sym)) return GdType.Obj(sym);
        return null;
    }

    // The one type every place agrees on, or the nearest class they share; null when a place has no usable type.
    private GdType? UnifyPlaces(List<ScenePlace> places)
    {
        GdType? acc = null;
        foreach (ScenePlace p in places)
        {
            GdType? t = PlaceType(p);
            if (t == null) return null;
            acc = acc == null ? t : CommonType(acc, t);
            if (acc == null) return null;
        }
        return acc;
    }

    private static GdType? CommonType(GdType a, GdType b)
    {
        if (a.Cs == b.Cs) return a;
        if (a.Kind == GK.ScriptClass && b.Kind == GK.ScriptClass)
            foreach (ScriptInfo s in a.Script!.Chain())
                if (b.Script!.DerivesFromScript(s)) return GdType.OfScript(s);
        Rs.INamedTypeSymbol? sa = a.Kind == GK.ScriptClass ? a.Script!.ApiRoot : a.Sym;
        Rs.INamedTypeSymbol? sb = b.Kind == GK.ScriptClass ? b.Script!.ApiRoot : b.Sym;
        if (sa == null || sb == null) return null;
        for (Rs.INamedTypeSymbol? t = sa; t != null; t = t.BaseType)
            if (GdType.Derives(sb, t)) return ApiModel.IsGodotDerived(t) && t.Name != "GodotObject" ? GdType.Obj(t) : null;
        return null;
    }

    /// <summary>GetNode&lt;T&gt;("path") when the scenes say what is there, else null. <paramref name="receiver"/> is the code of the node the path starts from ("" for this).</summary>
    private Ex? SceneNodeEx(string path, bool orNull, List<ScenePlace>? from, string receiver)
    {
        if (_m.Scenes == null || from == null || from.Count == 0) return null;
        var hits = new List<ScenePlace>();
        foreach (ScenePlace f in from)
            if (SceneIndex.Resolve(f, path) is { } p) hits.Add(p);
        if (hits.Count == 0) return null;
        GdType? t = UnifyPlaces(hits);
        if (t == null) return null;
        string prefix = receiver.Length > 0 ? receiver + "." : "";
        if (receiver.Length == 0) _ctx.UsedInstance = true;
        return new Ex($"{prefix}{(orNull ? "GetNodeOrNull" : "GetNode")}<{t.Cs}>({Naming.Quote(path)})", t.AsScene()) { Where = hits, UsesInstance = receiver.Length == 0 };
    }

    // get_node("literal") / get_node_or_null("literal") on this or on a node whose place is known.
    private bool TrySceneGetNode(string name, Ex? receiver, Call c, GdType? expected, out Ex? result)
    {
        result = null;
        if (c.Args.Count != 1 || c.Args[0] is not Literal { Kind: LitKind.String or LitKind.NodePath or LitKind.StringName } lit) return false;
        if (expected != null && expected.IsGodotObject && !expected.Weak && expected.Sym?.Name != "GodotObject") return false;
        bool self = receiver == null || receiver.Code == "this";
        List<ScenePlace>? from = self ? (ScriptIsNode() ? SelfPlaces() : null) : receiver!.Where;
        result = SceneNodeEx((string)lit.Value!, name == "get_node_or_null", from, self ? "" : Wrap(receiver!, P.Primary));
        if (result == null && self) result = CastPathEx((string)lit.Value!, name == "get_node_or_null");
        return result != null;
    }

    // ---- parameter types from the call sites ----
    // An untyped parameter is a Variant, and a member read on it (player_position.x) is a read by name. When the project calls the function with one
    // math type (a Vector3 every time), the parameter is that type: GDScript would fail on any other the first time the body used it as one.

    private Dictionary<MemberInfo, (GdType?[] Types, bool[] Bad)>? _observed;
    private readonly Dictionary<MemberInfo, GdType?[]> _hints = new();

    /// <summary>Emits every script once, only to see what its calls of the project's functions pass; the hints are then used by the real pass.</summary>
    public void CollectParamHints(IEnumerable<ScriptInfo> scripts)
    {
        _observed = new();
        try
        {
            foreach (ScriptInfo s in scripts) EmitScript(s);
            foreach (var (mi, o) in _observed)
                _hints[mi] = o.Types.Select((t, i) => o.Bad[i] ? null : t).ToArray();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException) { _hints.Clear(); }   // the hints only sharpen types; the real pass reports its own failures
        finally { _observed = null; }
        _notes.Clear();
        _noteTag = 0;
        _forced.Clear();
        _forceNew.Clear();
    }

    private void Observe(MemberInfo mi, IReadOnlyList<Expr> args)
    {
        FuncDecl fd = mi.Func!;
        if (!_observed!.TryGetValue(mi, out var o)) _observed[mi] = o = (new GdType?[fd.Params.Count], new bool[fd.Params.Count]);
        for (int i = 0; i < fd.Params.Count; i++)
        {
            if (o.Bad[i]) continue;
            GdType t = i < args.Count ? TypeOf(args[i]) : GdType.Variant;
            if (t.Kind != GK.Struct || t.Sym == null || fd.Params[i].IsRest) o.Bad[i] = true;
            else if (o.Types[i] == null) o.Types[i] = t;
            else if (o.Types[i]!.Cs != t.Cs) o.Bad[i] = true;
        }
    }

    private GdType? ParamHint(MemberInfo mi, int index, Param p) =>
        _hints.TryGetValue(mi, out GdType?[]? h) && index < h.Length && h[index] is { } t && !AssignsTo(mi.Func!.Body, p.Name) ? t : null;

    /// <summary>The scene a PackedScene-typed member holds when every scene that places the script sets it to the same .tscn.</summary>
    private string? PackedSceneOf(MemberInfo mi)
    {
        List<ScenePlace>? places = PlacementsOf(mi.Owner);
        if (places == null) return null;
        string? scene = null;
        foreach (ScenePlace p in places)
        {
            if (!p.Node.Packed.TryGetValue(mi.Name, out string? s)) continue;
            if (scene != null && scene != s) return null;
            scene = s;
        }
        return scene;
    }

    private string? SceneOfConst(MemberInfo mi) =>
        mi.Decl is ConstDecl { Value: PreloadExpr { Path: Literal { Kind: LitKind.String } lit } } && (string)lit.Value! is { } p && p.EndsWith(".tscn", StringComparison.Ordinal) ? p : null;

    // scene.instantiate() where the scene is known: Instantiate<T> of the scene's root, so what comes back is typed.
    private bool TryInstantiate(Ex target, Call c, GdType? expected, out Ex? result)
    {
        result = null;
        if (_m.Scenes == null || target.SceneRes == null || c.Args.Count != 0) return false;
        if (expected != null && expected.IsGodotObject && !expected.Weak && expected.Sym?.Name is not ("GodotObject" or "Node")) return false;
        if (_m.Scenes.Root(target.SceneRes) is not { } root) return false;
        GdType? t = PlaceType(root);
        if (t == null) return false;
        result = new Ex($"{Wrap(target, P.Primary)}.Instantiate<{t.Cs}>()", t.AsScene()) { Where = new List<ScenePlace> { root } };
        return true;
    }
}
