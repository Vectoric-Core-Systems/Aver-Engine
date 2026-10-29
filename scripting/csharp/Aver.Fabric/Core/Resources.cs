// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Resource, ResourceLoader and PackedScene. A converted project's assets sit in its content folder in the layout the converter
// wrote (docs/plans/fabric/G3_SCENES.md); res:// paths are mapped onto that layout. A scene is the level file the converter wrote
// for it, read into a node template that Instantiate builds fresh each time.

using System.Reflection;

namespace Aver.Fabric;

/// <summary>A shared data object with a path (Godot's Resource).</summary>
public class Resource : RefCounted
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName Changed = "changed";
    }

    public event Action Changed { add => Connect(SignalName.Changed, Callable.From(value)); remove => Disconnect(SignalName.Changed, Callable.From(value)); }

    public string ResourcePath { get; set; } = "";
    public string ResourceName { get; set; } = "";

    /// <summary>When true each scene instance gets its own copy of this resource instead of sharing it.</summary>
    public bool ResourceLocalToScene { get; set; }

    /// <summary>A copy that has no path. With <paramref name="subresources"/> the resources it holds are copied too; otherwise
    /// the copy shares them.</summary>
    public virtual Resource Duplicate(bool subresources = false)
    {
        var copy = (Resource)MemberwiseClone();
        copy.ResetIdentity();
        copy.ResourcePath = "";
        if (subresources)
        {
            foreach (PropertyInfo p in GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                if (!typeof(Resource).IsAssignableFrom(p.PropertyType) || !p.CanRead || !p.CanWrite || p.GetIndexParameters().Length != 0) continue;
                if (p.GetValue(this) is Resource inner) p.SetValue(copy, inner.Duplicate(true));
            }
        }
        return copy;
    }

    public virtual void EmitChanged() => EmitSignal(SignalName.Changed);

    /// <summary>Makes this resource the one loaded for <paramref name="path"/> (Godot's take_over_path).</summary>
    public void TakeOverPath(string path)
    {
        ResourcePath = path;
        ResourceLoader.Cache(path, this);
    }

    /// <summary>Called on a local-to-scene copy when its scene is instanced.</summary>
    public virtual void _SetupLocalToScene() { }

    public Rid GetRid() => new((ulong)GetInstanceId());
}

/// <summary>Loads resources by path and keeps them, so two loads of one path are the same object (Godot's ResourceLoader).</summary>
/// <remarks>
/// <c>res://a/b.png</c> names the file in the project's content folder: tried as written, then under <c>Textures/</c> (where the
/// converter put the textures); a scene (<c>.tscn</c>) is the level the converter wrote for it, <c>Maps/a/b.ocworld</c>. A path with
/// no file behind it still loads: the result is a stand-in of the requested type (or the type its extension suggests) that knows its
/// path, which is what a scene's <c>Resource("path")</c> value needs when the asset is loaded by the engine's own loaders instead.
/// <see cref="Exists"/> says whether there is a file.
/// </remarks>
public static class ResourceLoader
{
    public enum ThreadLoadStatus { InvalidResource = 0, InProgress = 1, Failed = 2, Loaded = 3 }

    public enum CacheMode { Ignore = 0, Reuse = 1, Replace = 2 }

    private static readonly Dictionary<string, Resource> s_cache = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, ThreadLoadStatus> s_threaded = new(StringComparer.Ordinal);

    // A loader a host or test installs: given a path and the wanted type, the resource or null.
    internal static Func<string, System.Type, Resource?>? Provider;

    // Where the content folder is. Null means the engine's (Paths.Content); tests point it at a folder.
    internal static string? ContentRootOverride;

    internal static string ContentRoot
    {
        get
        {
            if (ContentRootOverride != null) return ContentRootOverride;
            try { return Aver.Framework.Paths.Content; }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return ""; }
        }
    }

    /// <summary>The content-relative files a path may be, best first.</summary>
    internal static IEnumerable<string> Candidates(string path)
    {
        string rel = path.StartsWith("res://", StringComparison.Ordinal) ? path["res://".Length..] : path;
        string ext = System.IO.Path.GetExtension(rel).ToLowerInvariant();
        if (ext is ".tscn" or ".scn")
        {
            yield return SceneTree.ScenePathMapper(path);
            yield break;
        }
        yield return rel;
        yield return "Textures/" + rel;
        yield return "Audio/" + rel;   // G5 S1: the converter copies sound files under Audio/
    }

    internal static string? FindFile(string path)
    {
        string root = ContentRoot;
        if (root.Length == 0) return null;
        foreach (string c in Candidates(path))
        {
            string full = System.IO.Path.Combine(root, c.Replace('/', System.IO.Path.DirectorySeparatorChar));
            if (File.Exists(full)) return full;
        }
        return null;
    }

    /// <summary>True when the resource is loaded already, a loader provides it, or its file is in the content folder.</summary>
    public static bool Exists(string path, string typeHint = "") =>
        s_cache.ContainsKey(path) || FindFile(path) != null || (Provider != null && Provider(path, typeof(Resource)) != null);

    // The class a file's extension stands for.
    internal static System.Type TypeForExtension(string ext) => ext.ToLowerInvariant() switch
    {
        ".tscn" or ".scn" or ".ocworld" => typeof(PackedScene),
        ".ocmesh" => typeof(Mesh),
        ".ocmat" => typeof(StandardMaterial3D),
        ".ocanim" => typeof(Animation),
        ".octheme" => typeof(Theme),
        ".ttf" or ".otf" or ".woff" or ".woff2" => typeof(FontFile),
        ".ocbuttongroup" => typeof(ButtonGroup),   // G5 U2: a button group the converter names; one path is one shared group
        ".wav" => typeof(AudioStreamWav),
        ".ogg" => typeof(AudioStreamOggVorbis),
        ".mp3" => typeof(AudioStreamMP3),
        ".png" or ".jpg" or ".jpeg" or ".webp" or ".bmp" or ".tga" or ".dds" or ".exr" or ".hdr" or ".svg" => typeof(Texture2D),
        _ => typeof(Resource),
    };

    public static Resource? Load(string path, System.Type? type = null, CacheMode cacheMode = CacheMode.Reuse)
    {
        type ??= typeof(Resource);
        if (cacheMode == CacheMode.Reuse && s_cache.TryGetValue(path, out Resource? hit) && type.IsInstanceOfType(hit)) return hit;
        Resource? r = Provider?.Invoke(path, type);
        if (r == null)
        {
            string? file = FindFile(path);
            string ext = System.IO.Path.GetExtension(path);
            if (file != null && TypeForExtension(ext) == typeof(PackedScene) && type.IsAssignableFrom(typeof(PackedScene)))
                r = PackedScene.FromFile(file, path);
            else
            {
                System.Type made = TypeForExtension(ext);
                if (!type.IsAssignableFrom(made)) made = type;
                if (typeof(Resource).IsAssignableFrom(made) && !made.IsAbstract && made.GetConstructor(System.Type.EmptyTypes) != null)
                    r = (Resource)Activator.CreateInstance(made)!;
                if (r is Animation anim && file != null && !anim.LoadFrom(file))
                    Aver.Scripting.Log.Warn($"[Fabric] cannot read the animation '{path}'");
                if (r is Theme theme && file != null) theme.LoadFrom(file);   // G5 U1: a malformed theme is reported there and stays empty
                if (r is ButtonGroup group && path.EndsWith("_unpress.ocbuttongroup", StringComparison.Ordinal)) group.AllowUnpress = true;
            }
        }
        if (r == null) return null;
        r.ResourcePath = path;
        if (cacheMode != CacheMode.Ignore) s_cache[path] = r;
        return r;
    }

    public static T? Load<T>(string path, CacheMode cacheMode = CacheMode.Reuse) where T : class => Load(path, typeof(T), cacheMode) as T;

    /// <summary>GDScript's preload: loads once and keeps it; a path with no file behind it is an error.</summary>
    internal static T Preload<T>(string path) where T : class
    {
        if (!Exists(path)) Aver.Scripting.Log.Error($"[Fabric] preload: there is no resource at '{path}'");
        return Load(path, typeof(T)) as T ?? throw new InvalidOperationException($"Cannot preload '{path}' as {typeof(T).Name}.");
    }

    /// <summary>The resource if it is loaded already, else null.</summary>
    public static Resource? GetCachedRef(string path) => s_cache.TryGetValue(path, out Resource? r) ? r : null;

    public static bool HasCached(string path) => s_cache.ContainsKey(path);

    internal static void Cache(string path, Resource r) => s_cache[path] = r;

    // ---- loading "in the background" ----
    // The load runs at the end of the current pass (the resources are plain objects that are made on the game thread); a script that
    // polls the status sees InProgress until then. Fetching early loads at once, as Godot's blocking get does.

    public static Error LoadThreadedRequest(string path, string typeHint = "", bool useSubThreads = false, CacheMode cacheMode = CacheMode.Reuse)
    {
        if (s_threaded.TryGetValue(path, out ThreadLoadStatus st) && st != ThreadLoadStatus.InvalidResource) return Error.Ok;
        s_threaded[path] = ThreadLoadStatus.InProgress;
        Aver.Framework.Deferred.Call(() => FinishThreaded(path, cacheMode));
        return Error.Ok;
    }

    private static void FinishThreaded(string path, CacheMode cacheMode)
    {
        if (!s_threaded.TryGetValue(path, out ThreadLoadStatus st) || st != ThreadLoadStatus.InProgress) return;
        Resource? r = Load(path, null, cacheMode);
        s_threaded[path] = r != null ? ThreadLoadStatus.Loaded : ThreadLoadStatus.Failed;
    }

    public static ThreadLoadStatus LoadThreadedGetStatus(string path, Collections.Array? progress = null)
    {
        if (!s_threaded.TryGetValue(path, out ThreadLoadStatus st)) return ThreadLoadStatus.InvalidResource;
        if (progress != null) { progress.Clear(); progress.Add(st == ThreadLoadStatus.Loaded ? 1.0 : 0.0); }
        return st;
    }

    public static Resource? LoadThreadedGet(string path)
    {
        if (!s_threaded.TryGetValue(path, out ThreadLoadStatus st)) return null;
        if (st == ThreadLoadStatus.InProgress) FinishThreaded(path, CacheMode.Reuse);
        s_threaded.Remove(path);
        return GetCachedRef(path);
    }

    internal static void ClearCache()
    {
        s_cache.Clear();
        s_threaded.Clear();
    }
}

/// <summary>A scene stored as a resource (Godot's PackedScene): the level file the converter wrote for a scene, read into a template.
/// <see cref="Instantiate"/> builds a fresh, detached node tree from it each time.</summary>
public class PackedScene : Resource
{
    public enum GenEditState { Disabled = 0, Instance = 1, Main = 2, MainInherited = 3 }

    private List<SceneNodeData>? _data;
    private Node? _template;

    /// <summary>True for a scene read from a converted level (the engine can travel to it as a level).</summary>
    internal bool IsLevelBacked => _data != null;

    internal static PackedScene FromFile(string file, string resPath)
    {
        var scene = new PackedScene { ResourcePath = resPath };
        try
        {
            scene._data = OcWorldScene.Parse(File.ReadAllText(file));
        }
        catch (Exception ex) when (ex is FormatException or IOException)
        {
            Aver.Scripting.Log.Error($"[Fabric] cannot read the scene '{resPath}': {ex.Message}");
        }
        return scene;
    }

    /// <summary>A scene from the text of a converted level file.</summary>
    internal static PackedScene FromText(string levelText, string resPath = "") =>
        new() { ResourcePath = resPath, _data = OcWorldScene.Parse(levelText) };

    public bool CanInstantiate() => (_data != null && _data.Any(n => !n.Persistent)) || _template != null;

    /// <summary>Stores a copy of a node and its children as this scene.</summary>
    public Error Pack(Node path)
    {
        ArgumentNullException.ThrowIfNull(path);
        _template = path.Duplicate();
        _data = null;
        return Error.Ok;
    }

    /// <summary>A new node tree from the scene, not in the tree yet; add it with AddChild. Its nodes have their saved names, properties,
    /// groups and signal connections applied as they enter the tree, before their _EnterTree.</summary>
    public virtual Node Instantiate(GenEditState editState = GenEditState.Disabled)
    {
        Node? root = null;
        if (_template != null) root = _template.Duplicate();
        else if (_data != null)
        {
            // Autoloads (persistent placements) belong to the project, not to the scene they were written beside.
            List<SceneNodeData> roots = _data.Where(n => !n.Persistent).ToList();
            if (roots.Count > 1)
                Aver.Scripting.Log.Warn($"[Fabric] the scene '{ResourcePath}' has {roots.Count} top-level nodes; instancing the first");
            if (roots.Count > 0) root = Build(roots[0]);
        }
        if (root == null)
        {
            Aver.Scripting.Log.Error($"[Fabric] cannot instantiate '{ResourcePath}': the scene has no nodes");
            return null!;
        }
        if (ResourcePath.Length > 0) root.SceneFilePath = ResourcePath;
        root.Notification(Node.NotificationSceneInstantiated);
        return root;
    }

    public T Instantiate<T>(GenEditState editState = GenEditState.Disabled) where T : Node => (T)Instantiate(editState);

    private static Node Build(SceneNodeData d)
    {
        System.Type? known = ScriptClasses.Find(d.Class);
        if (known == null && d.Class.Length > 0)
            Aver.Scripting.Log.Warn($"[Fabric] class '{d.Class}' of node '{d.Name}' is not known; a Node3D stands in");
        // A placement with no class is a plain mesh placement (or a marker when it names no mesh).
        System.Type type = known ?? (d.Class.Length == 0 && d.Asset != "none" ? typeof(MeshInstance3D) : typeof(Node3D));
        var node = (Node)Activator.CreateInstance(type)!;
        if (d.Name.Length > 0) node.Name = d.Name;
        if (node is Node3D n3) n3.Transform = d.LocalTransform();
        var values = new List<(string Key, string Text)>(d.Props);
        if (d.Class.Length == 0 && d.Asset != "none") values.Add(("mesh", $"Resource(\"{d.Asset}\")"));
        node.PendingValues = values;
        foreach (string g in d.Groups) node.AddToGroup(g);
        foreach (SceneNodeData c in d.Children) node.AddChild(Build(c));
        return node;
    }
}

// Script classes by the name a level gives them (the registry name: the type name, or [AverClass("Name")]).
internal static class ScriptClasses
{
    private static readonly Dictionary<string, System.Type> s_byName = new(StringComparer.Ordinal);
    private static int s_assemblies = -1;

    internal static System.Type? Find(string name)
    {
        if (name.Length == 0) return null;
        System.Type? t = ClassDB.TypeOf(name);
        if (t != null && typeof(Node).IsAssignableFrom(t)) return t;
        lock (s_byName)
        {
            if (s_byName.TryGetValue(name, out System.Type? hit)) return hit;
            Assembly[] all = AppDomain.CurrentDomain.GetAssemblies();
            if (all.Length != s_assemblies)
            {
                s_assemblies = all.Length;
                foreach (Assembly a in all)
                {
                    if (a.IsDynamic) continue;
                    System.Type[] types;
                    try { types = a.GetTypes(); }
                    catch (ReflectionTypeLoadException ex) { types = ex.Types.Where(x => x != null).ToArray()!; }
                    catch (Exception) { continue; }   // an assembly that cannot be read has no script classes we can use
                    foreach (System.Type c in types)
                        if (typeof(Node).IsAssignableFrom(c) && !c.IsAbstract && c.GetConstructor(System.Type.EmptyTypes) != null)
                            s_byName.TryAdd(Aver.Framework.ClassNames.Of(c), c);
                }
            }
            return s_byName.TryGetValue(name, out System.Type? found) ? found : null;
        }
    }
}
