// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Loads the native Godot plugins (.gdextension) of a converted project through the Aver GDExtension host (modules/fabric.gdext,
// Aver.Fabric.GDExt.Abi) and makes their classes available to ClassDB. docs/plans/fabric/G3_GDEXT.md says what the host implements.

using Aver.Scripting;

namespace Aver.Fabric;

/// <summary>Godot's GDExtensionManager for a converted project: <see cref="Load"/> a plugin, then
/// <c>ClassDB.Instantiate("TheClass")</c> or a level's <c>GDExtensionNode</c> placement uses its classes.</summary>
public static class GDExtensionManager
{
    private static readonly Dictionary<string, int> s_libraries = new(StringComparer.Ordinal);
    private static int s_available = -1;

    /// <summary>True when the native host library (Aver.Fabric.GDExt.Abi) can be loaded and speaks this ABI version.</summary>
    public static bool IsNativeAvailable
    {
        get
        {
            if (s_available < 0)
            {
                try { s_available = GdextNative.aver_gdext_abi_version() >> 16 == 1 ? 1 : 0; }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
                {
                    s_available = 0;
                    LastError = "the GDExtension host library (Aver.Fabric.GDExt.Abi) is not available: " + ex.Message;
                }
            }
            return s_available == 1;
        }
    }

    /// <summary>Why the last call that returned false failed.</summary>
    public static string LastError { get; private set; } = "";

    /// <summary>The folder res:// library paths of .gdextension files resolve against; the loaded project's folder.</summary>
    public static string? ProjectDirectory { get; set; }

    private static string Normalize(string path) => Path.GetFullPath(path).Replace('\\', '/');

    private static bool Fail(string message) { LastError = message; Log.Error("[Fabric] GDExtension: " + message); return false; }

    /// <summary>The extension_api.json of the Godot version the plugins target, for the method-hash checks (optional).</summary>
    public static bool SetApiPath(string path) => IsNativeAvailable && GdextNative.aver_gdext_set_api_path(path) == 1;

    /// <summary>Loads a .gdextension file: picks this platform's library, runs the plugin's entry function and its initialization.
    /// Refuses extensions for Godot 4.0 (compatibility_minimum below 4.1). Loading a path twice is a no-op that returns true.</summary>
    public static bool Load(string gdextensionPath, string? projectDirectory = null)
    {
        if (!IsNativeAvailable) return Fail(LastError.Length > 0 ? LastError : "the GDExtension host library is not available");
        string full = Normalize(gdextensionPath);
        if (s_libraries.ContainsKey(full)) return true;
        int handle = GdextNative.aver_gdext_load(full, projectDirectory ?? ProjectDirectory);
        if (handle == 0) return Fail(GdextNative.LastError());
        s_libraries[full] = handle;
        return true;
    }

    public static bool IsLoaded(string gdextensionPath) => s_libraries.ContainsKey(Normalize(gdextensionPath));

    private static bool s_projectScanned;

    /// <summary>Loads every <c>.gdextension</c> file under the project's content folder (where the converter copied the plugins, at
    /// their <c>res://</c> paths). <paramref name="contentDirectory"/> defaults to <see cref="Aver.Framework.Paths.Content"/>. Returns
    /// how many loaded now; a file that fails is logged and skipped.</summary>
    public static int LoadProjectExtensions(string? contentDirectory = null)
    {
        string root = contentDirectory ?? "";
        if (root.Length == 0)
        {
            try { root = Aver.Framework.Paths.Content; }
            catch (Exception) { root = ""; }   // no engine behind the framework (a test host, a tool): no project folder
        }
        if (root.Length == 0 || !Directory.Exists(root)) return 0;
        int loaded = 0;
        foreach (string file in Directory.EnumerateFiles(root, "*.gdextension", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.Ordinal))
        {
            if (IsLoaded(file)) continue;
            if (Load(file, root)) loaded++;
        }
        return loaded;
    }

    /// <summary>The first time a class is asked for that no loaded plugin has, the project's own plugins are loaded (once).</summary>
    internal static void EnsureProjectExtensions()
    {
        if (s_projectScanned) return;
        s_projectScanned = true;
        LoadProjectExtensions();
    }

    /// <summary>Deinitializes the plugin, frees its objects and classes and unloads the library.</summary>
    public static bool Unload(string gdextensionPath)
    {
        string full = Normalize(gdextensionPath);
        if (!s_libraries.TryGetValue(full, out int handle)) return Fail("not loaded: " + gdextensionPath);
        s_libraries.Remove(full);
        if (GdextNative.aver_gdext_unload(handle) != 1) return Fail(GdextNative.LastError());
        return true;
    }

    /// <summary>The .gdextension paths loaded so far.</summary>
    public static string[] GetLoadedExtensions() => s_libraries.Keys.ToArray();

    internal static bool AnyLoaded => s_libraries.Count > 0;

    /// <summary>The classes the loaded plugins registered, in registration order.</summary>
    public static string[] GetClassList()
    {
        if (!AnyLoaded) return System.Array.Empty<string>();
        int n = GdextNative.aver_gdext_class_count(0);
        var names = new string[n];
        for (int i = 0; i < n; i++) names[i] = GdextNative.ClassName(0, i);
        return names;
    }

    /// <summary>True for a class a loaded plugin registered.</summary>
    public static bool ClassExists(string className) => AnyLoaded && GdextNative.aver_gdext_class_exists(className) == 1;

    public static string GetParentClass(string className) => AnyLoaded ? GdextNative.ClassParent(className) : "";

    public static bool CanInstantiate(string className) => ClassExists(className) && GdextNative.aver_gdext_class_can_instantiate(className) == 1;

    /// <summary>The methods the plugin's class registered itself.</summary>
    public static string[] GetMethodList(string className)
    {
        if (!AnyLoaded) return System.Array.Empty<string>();
        int n = GdextNative.aver_gdext_method_count(className);
        var names = new string[n];
        for (int i = 0; i < n; i++) names[i] = GdextNative.MethodName(className, i);
        return names;
    }

    public static string[] GetPropertyList(string className)
    {
        if (!AnyLoaded) return System.Array.Empty<string>();
        int n = GdextNative.aver_gdext_property_count(className);
        var names = new string[n];
        for (int i = 0; i < n; i++) names[i] = GdextNative.PropertyName(className, i);
        return names;
    }

    public static string[] GetSignalList(string className)
    {
        if (!AnyLoaded) return System.Array.Empty<string>();
        int n = GdextNative.aver_gdext_signal_count(className);
        var names = new string[n];
        for (int i = 0; i < n; i++) names[i] = GdextNative.SignalName(className, i);
        return names;
    }

    /// <summary>The integer constants the plugin's class registered.</summary>
    public static IReadOnlyDictionary<string, long> GetConstants(string className)
    {
        var d = new Dictionary<string, long>();
        if (!AnyLoaded) return d;
        for (int i = 0, n = GdextNative.aver_gdext_constant_count(className); i < n; i++)
            d[GdextNative.ConstantName(className, i)] = GdextNative.aver_gdext_constant_value(className, i);
        return d;
    }

    /// <summary>Interface functions and engine methods plugins asked for that the host does not implement (first asked first).</summary>
    public static string[] GetUnsupportedCalls()
    {
        if (!IsNativeAvailable) return System.Array.Empty<string>();
        int n = GdextNative.aver_gdext_unsupported_count();
        var names = new string[n];
        for (int i = 0; i < n; i++) names[i] = GdextNative.UnsupportedName(i);
        return names;
    }

    /// <summary>Makes an object of a plugin's class: a <see cref="GDExtensionNode3D"/>, a <see cref="GDExtensionNode"/> or a
    /// <see cref="GDExtensionObject"/> by what the class extends. Null when the class is unknown or abstract.</summary>
    public static GodotObject? Instantiate(string className)
    {
        if (!ClassExists(className) && IsNativeAvailable) EnsureProjectExtensions();
        if (!CanInstantiate(className)) return null;
        if (GdextNative.aver_gdext_class_is_a(className, "Node3D") != 0)
        {
            var n = new GDExtensionNode3D { ExtensionClass = className };
            return n.EnsureNative() ? n : null;
        }
        if (GdextNative.aver_gdext_class_is_a(className, "Node") != 0)
        {
            var n = new GDExtensionNode { ExtensionClass = className };
            return n.EnsureNative() ? n : null;
        }
        var o = new GDExtensionObject(className);
        return o.NativeHandle != 0 ? o : null;
    }

    // ---- Variant <-> native objects (used by VariantWire) ----

    internal static long NativeInstanceId(GodotObject o) => GdextPeer.Of(o) is { } p ? p.InstanceId : 0;

    internal static GodotObject? ObjectFromNativeId(long id)
    {
        if (id == 0 || !AnyLoaded) return null;
        int handle = GdextNative.aver_gdext_object_by_instance_id(id);
        if (handle == 0) return null;
        if (GdextPeer.ByHandle(handle) is { } peer) return peer.Owner;
        string cls = GdextNative.ObjectClass(handle);
        return GdextNative.aver_gdext_class_is_a(cls, "Node3D") != 0 ? GDExtensionNode3D.AdoptNative(handle)
             : GdextNative.aver_gdext_class_is_a(cls, "Node") != 0 ? GDExtensionNode.AdoptNative(handle) : null;
    }
}
